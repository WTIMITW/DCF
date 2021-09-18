/*
 * Copyright (c) 2021 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * rep_leader.c
 *    leader  process
 *
 * IDENTIFICATION
 *    src/replication/rep_leader.c
 *
 * -------------------------------------------------------------------------
 */

#include "rep_leader.h"
#include "cm_date.h"
#include "cm_thread.h"
#include "metadata.h"
#include "election.h"
#include "rep_msg_pack.h"
#include "rep_common.h"
#include "replication.h"
#include "cm_timer.h"
#include "util_perf_stat.h"
#include "rep_monitor.h"

#define APPEND_NORMAL_MODE 0
#define APPEND_REMATCH_MODE 1
#define APPEND_INTERVAL 1000    // ms

#define FLAG_EXISTS_ACTIVE_NODE 0x1
#define FLAG_EXISTS_LOG         0x2
#define FLAG_CONTROL_FLOW       0x4

typedef struct st_rep_leader_state_t {
    volatile uint64     next_index[CM_MAX_NODE_COUNT];
    volatile log_id_t   match_index[CM_MAX_NODE_COUNT];
    volatile uint8      append_mode[CM_MAX_NODE_COUNT];
    volatile uint32     pause_time[CM_MAX_NODE_COUNT];
    atomic32_t          try_rematch[CM_MAX_NODE_COUNT];
    uint64              apply_index[CM_MAX_NODE_COUNT];
    uint64              pre_app_time[CM_MAX_NODE_COUNT];
    uint32              disk_error;
}rep_leader_state_t;

#define NEXT_INDEX  (g_leader_state[stream_id].next_index[node_id])
#define MATCH_INDEX (g_leader_state[stream_id].match_index[node_id])
#define APPEND_MODE (g_leader_state[stream_id].append_mode[node_id])
#define PAUSE_TIME  (g_leader_state[stream_id].pause_time[node_id])
#define TRY_REMATCH (g_leader_state[stream_id].try_rematch[node_id])
#define APPLY_INDEX (g_leader_state[stream_id].apply_index[node_id])
#define PRE_APPTIME (g_leader_state[stream_id].pre_app_time[node_id])
#define DISK_ERROR  (g_leader_state[stream_id].disk_error)
#define DISK_ERROR_THRESHOLD 10
// leader state
rep_leader_state_t    g_leader_state[CM_MAX_STREAM_COUNT];

static cm_thread_cond_t g_appendlog_cond;
static thread_t g_appendlog_thread[REP_MAX_APPEND_THREAS_NUM];
static uint64 g_append_thread_id[CM_MAX_NODE_COUNT];
static uint32 g_append_thread_num;
static uint32 g_cur_node_id;

// for monitor
thread_t g_leader_monitor_thread;
rep_monitor_statistics_t g_leader_monitor_statistics;
#define LOAD_LEVEL          (g_leader_monitor_statistics.load_level)
#define ADJUST_STEP         (g_leader_monitor_statistics.adjust_step)
#define HIGH_LEVEL_TIMES    (g_leader_monitor_statistics.high_level_times)

#define REP_FC_TIME_UNIT       100  // 100us unit
#define REP_FC_INIT_VAL        5
#define REP_FC_SAMP_PERIOD     1
#define REP_FC_CTRL_PERIOD     5
#define REP_FC_MAX_VAL         100
static volatile uint32 g_rep_flow_ctrl_val = REP_FC_INIT_VAL;
static uint32 g_flow_ctrl_type = FC_NONE;


static void rep_appendlog_thread_entry(thread_t *thread);
static status_t rep_appendlog_ack_proc(mec_message_t *pack);
static void rep_follower_accepted_trigger(uint32 stream_id, uint32 node_id, log_id_t log_id);
static void rep_leader_monitor_entry(thread_t *thread);
static void rep_init_thread_id();
status_t rep_leader_reset(uint32 stream_id);
status_t rep_wait_all_logs_applied(uint32 stream_id);

// called when module is started
status_t rep_leader_init()
{
    uint32 streams[CM_MAX_STREAM_COUNT];
    uint32 stream_count;
    param_value_t param_value;
    g_cur_node_id = md_get_cur_node();

    cm_init_cond(&g_appendlog_cond);

    register_msg_process(MEC_CMD_APPEND_LOG_RPC_ACK, rep_appendlog_ack_proc, PRIV_LOW);

    if (md_get_param(DCF_PARAM_MEC_BATCH_SIZE, &param_value) != CM_SUCCESS) {
        LOG_RUN_ERR("rep_leader_init: get batchsize failed.");
        return CM_ERROR;
    }
    if (param_value.batch_size == 0) {
        g_flow_ctrl_type = FC_COMMIT_DELAY;
    }
    LOG_RUN_INF("rep_leader_init: flow_ctrl_type=%u.", g_flow_ctrl_type);

    if (md_get_param(DCF_REP_APPEND_THREAD_NUM, &param_value) != CM_SUCCESS) {
        return CM_ERROR;
    }

    g_append_thread_num = param_value.rep_append_thread_num;
    if (g_append_thread_num <= 0 || g_append_thread_num > REP_MAX_APPEND_THREAS_NUM) {
        LOG_RUN_ERR("rep_leader_init failed: invalid param value :REP_APPEND_THREAD_NUM = %u.",
            g_append_thread_num);
        return CM_ERROR;
    }
    rep_init_thread_id();

    CM_RETURN_IFERR(md_get_stream_list(streams, &stream_count));
    for (uint32 i = 0; i < stream_count; i++) {
        uint32 stream_id = streams[i];
        CM_RETURN_IFERR(rep_leader_reset(stream_id));
    }

    CM_RETURN_IFERR(rep_monitor_init());
    CM_RETURN_IFERR(cm_create_thread(rep_leader_monitor_entry, SIZE_M(CM_2X_FIXED), NULL, &g_leader_monitor_thread));

    for (uint64 i = 0; i < g_append_thread_num; i++) {
        CM_RETURN_IFERR(cm_create_thread(rep_appendlog_thread_entry, SIZE_M(CM_2X_FIXED),
            (void*)i, &g_appendlog_thread[i]));
    }

    for (uint32 i = 0; i < stream_count; i++) {
        uint32 stream_id = streams[i];
        if (I_AM_LEADER(stream_id)) {
            (void)set_node_status(stream_id, NODE_BLOCKED, 0);
            /* new leader must wait all logs applied and then set can_write flag */
            CM_RETURN_IFERR(rep_wait_all_logs_applied(stream_id));
            rep_set_can_write_flag(stream_id, CM_TRUE);
        }
    }

    LOG_RUN_INF("rep_leader_init finished");

    return CM_SUCCESS;
}

void rep_leader_deinit()
{
    for (uint64 i = 0; i < g_append_thread_num; i++) {
        cm_close_thread(&g_appendlog_thread[i]);
    }
    cm_close_thread(&g_leader_monitor_thread);
    LOG_RUN_INF("rep_leader_deinit finished");
}

status_t rep_wait_all_logs_applied(uint32 stream_id)
{
    uint64 last_index = rep_get_last_index(stream_id);
    uint64 applied_index = stg_get_applied_index(stream_id);
    date_t begin = g_timer()->now;
    date_t last = g_timer()->now;
    while (last_index != applied_index) {
        if ((g_timer()->now - last) > MICROSECS_PER_SECOND) {
            LOG_RUN_INF("[REP]already wait for %lld seconds,last_index=%llu,applied_index=%llu",
                        (g_timer()->now - begin) / MICROSECS_PER_SECOND, last_index, applied_index);
            last = g_timer()->now;
        }
        cm_sleep(1);
        if (!I_AM_LEADER(stream_id)) {
            LOG_RUN_INF("[REP]wait_all_logs_applied:I'm not leader now.");
            return CM_ERROR;
        }
        if (elc_is_notify_thread_closed() == CM_TRUE) {
            LOG_RUN_INF("[REP]status_notify_thread closed, stop now.");
            return CM_ERROR;
        }
        last_index = rep_get_last_index(stream_id);
        applied_index = stg_get_applied_index(stream_id);
    }

    LOG_DEBUG_INF("[REP]wait_all_logs_applied OK. last_index=%llu, applied_index=%llu", last_index, applied_index);
    return CM_SUCCESS;
}

// called by election when this node becomes leader
status_t rep_leader_reset(uint32 stream_id)
{
    uint32 nodes[CM_MAX_NODE_COUNT];
    uint32 count;

    CM_RETURN_IFERR(md_get_stream_nodes(stream_id, nodes, &count));

    log_id_t last_log = stg_last_log_id(stream_id);
    for (uint32 i = 0; i < count; i++) {
        uint32 node_id = nodes[i];
        NEXT_INDEX = last_log.index;
        if (node_id == g_cur_node_id) {
            MATCH_INDEX = stg_last_disk_log_id(stream_id);
        } else {
            log_id_t* invalid_log_id = get_invalid_log_id();
            MATCH_INDEX = *invalid_log_id;
        }
        APPEND_MODE = APPEND_NORMAL_MODE;
        TRY_REMATCH = CM_FALSE;
        PRE_APPTIME = 0;
        PAUSE_TIME  = 0;
        LOG_DEBUG_INF("[REP]rep_leader_reset:node_id=%u,next_index=%llu", node_id,
            NEXT_INDEX);
    }

    if (I_AM_LEADER(stream_id)) {
        /* Write matadata when leader reset for:
        1. try commit previous term's log
        2. ensure configurations on all nodes are consistent */
        CM_RETURN_IFERR(md_set_status(META_CATCH_UP));
        uint32 size;
        char *md_buf = (char *)malloc(CM_METADATA_DEF_MAX_LEN);
        if (md_buf == NULL) {
            LOG_DEBUG_ERR("rep_leader_reset malloc failed");
            CM_RETURN_IFERR(md_set_status(META_NORMAL));
            return CM_ERROR;
        }
        if (md_to_string(md_buf, CM_METADATA_DEF_MAX_LEN, &size) != CM_SUCCESS) {
            CM_FREE_PTR(md_buf);
            CM_RETURN_IFERR(md_set_status(META_NORMAL));
            return CM_ERROR;
        }
        if (rep_write(stream_id, md_buf, size, 0, ENTRY_TYPE_CONF, NULL) != CM_SUCCESS) {
            CM_FREE_PTR(md_buf);
            CM_RETURN_IFERR(md_set_status(META_NORMAL));
            return CM_ERROR;
        }
        CM_FREE_PTR(md_buf);
        CM_RETURN_IFERR(md_set_status(META_NORMAL));
    } else {
        LOG_RUN_WAR("rep_leader_reset:I'm not a leader now!");
    }

    LOG_RUN_INF("rep_leader_reset finished");

    return CM_SUCCESS;
}

static void rep_release_entrys(log_entry_t** entrys, uint64 count)
{
    for (uint64 j = 0; j < count; j++) {
        if (entrys[j] != NULL) {
            stg_entry_dec_ref(entrys[j]);
        }
    }
}

static inline void rep_init_appendlog_req(uint32 stream_id, rep_apendlog_req_t* appendlog_req,
    uint64 pre_log_index, uint64 last_log_index, uint64 log_count)
{
    appendlog_req->head.req_seq = g_timer()->now;
    appendlog_req->head.ack_seq = 0;
    appendlog_req->head.trace_key = get_trace_key();
    appendlog_req->head.msg_ver = REP_MSG_VER;
    appendlog_req->leader_node_id = g_cur_node_id;
    appendlog_req->leader_term = elc_get_current_term(stream_id);
    appendlog_req->leader_commit_log = rep_get_commit_log(stream_id);
    appendlog_req->leader_first_log.index = stg_first_index(stream_id);
    appendlog_req->leader_first_log.term = stg_get_term(stream_id, appendlog_req->leader_first_log.index);
    appendlog_req->pre_log.index = pre_log_index;
    appendlog_req->pre_log.term = stg_get_term(stream_id, pre_log_index);
    appendlog_req->leader_last_index = last_log_index;
    appendlog_req->cluster_min_apply_id = rep_get_cluster_min_apply_idx(stream_id);
    appendlog_req->log_count = log_count;
}

static uint64 rep_calu_log_count_by_control(dcf_role_t default_role, uint64 log_count)
{
    if (default_role != DCF_ROLE_PASSIVE) {
        return log_count;
    }
    LOG_DEBUG_INF("[REP]before control count: %llu", log_count);
    if (log_count == 0) {
        return log_count;
    }
    log_count = (uint64)(log_count * ADJUST_STEP);
    log_count = log_count > REP_MAX_LOG_COUNT ? REP_MAX_LOG_COUNT : log_count;
    LOG_DEBUG_INF("[REP]flow control count: %llu, load level: %d, step: %f, high times: %u", log_count, LOAD_LEVEL,
                  ADJUST_STEP, HIGH_LEVEL_TIMES);
    if (log_count == 0) {
        return 1;
    }

    return log_count;
}

static uint64 rep_calu_log_count(uint32 stream_id, uint32 node_id, dcf_role_t default_role, uint64 log_begin,
    uint64 log_end)
{
    uint64 log_count;

    if (log_end == CM_INVALID_INDEX_ID) {
        return 0;
    }

    if (log_end < log_begin) {
        return 0;
    }

    if (log_begin == CM_INVALID_INDEX_ID) {
        log_count = log_end;
    } else {
        log_count = log_end - log_begin + 1;
    }

    if (APPEND_MODE == APPEND_NORMAL_MODE) {
        log_count = MIN(log_count, REP_MAX_LOG_COUNT);
    } else {
        log_count = MIN(log_count, 1);
    }

    return rep_calu_log_count_by_control(default_role, log_count);
}

// Check if value illegal at compile time
CM_STATIC_ASSERT((MEC_BUFFER_RESV_SIZE - PADDING_BUFFER_SIZE) >= sizeof(rep_apendlog_req_t));
CM_STATIC_ASSERT((MEC_BUFFER_RESV_SIZE - PADDING_BUFFER_SIZE) < (sizeof(rep_apendlog_req_t) + SIZE_K(1)));

#define FILL_APPEND_LOG_REQ(appendlog_req, index, log_begin, j)                               \
    do {                                                                                      \
        (appendlog_req)->logs[(index) - (log_begin)].log_id.term = ENTRY_TERM(entrys[(j)]);   \
        (appendlog_req)->logs[(index) - (log_begin)].log_id.index = ENTRY_INDEX(entrys[(j)]); \
        (appendlog_req)->logs[(index) - (log_begin)].buf = ENTRY_BUF(entrys[(j)]);            \
        (appendlog_req)->logs[(index) - (log_begin)].size = ENTRY_SIZE(entrys[(j)]);          \
        (appendlog_req)->logs[(index) - (log_begin)].type = ENTRY_TYPE(entrys[(j)]);          \
        (appendlog_req)->logs[(index) - (log_begin)].key = ENTRY_KEY(entrys[(j)]);            \
    } while (0)

static status_t rep_appendlog_node(uint32 stream_id, uint32 node_id, dcf_role_t default_role, uint64 last_log_index,
    bool8* node_exists_log)
{
    LOG_DEBUG_INF("rep_appendlog_node begin");
    rep_apendlog_req_t* appendlog_req = (rep_apendlog_req_t*)rep_get_appenlog_req_buf(sizeof(rep_apendlog_req_t));
    CM_CHECK_NULL_PTR(appendlog_req);
    log_entry_t** entrys = (log_entry_t**)rep_get_entrys_buf(sizeof(log_entry_t*)*REP_MAX_LOG_COUNT);
    CM_CHECK_NULL_PTR(entrys);
    uint64 old_next_index = (uint64)cm_atomic_get((atomic_t*)&NEXT_INDEX);
    uint64 log_begin = old_next_index == CM_INVALID_INDEX_ID ? 1 : old_next_index;
    log_begin = MAX(log_begin, stg_first_index(stream_id));
    uint64 log_count = rep_calu_log_count(stream_id, node_id, default_role, log_begin, last_log_index);
    uint64 j = 0;
    uint32 total_size = 0;
    *node_exists_log = (log_count > 0);

    for (uint64 index = log_begin; j < log_count; index++, j++) {
        entrys[j] = stg_get_entry(stream_id, index);
        if (entrys[j] == NULL) {
            break;
        }
        total_size += ENTRY_SIZE(entrys[j]);
        if (total_size > MESSAGE_BUFFER_SIZE && j > 0) {
            LOG_DEBUG_INF("total_size[%u] is enough, send size[%u]. log_count[%llu], j[%llu]",
                total_size, total_size - ENTRY_SIZE(entrys[j]), log_count, j);
            stg_entry_dec_ref(entrys[j]);
            break;
        }
        ps_record1(PS_PACK, index);
        FILL_APPEND_LOG_REQ(appendlog_req, index, log_begin, j);
    }

    /* Logs are sent even if log_count==0.
    Periodically sending empty logs ensures that lost packets are retransmitted */
    mec_message_t pack;
    CM_RETURN_IFERR_EX(mec_alloc_pack(&pack, MEC_CMD_APPEND_LOG_RPC_REQ, g_cur_node_id, node_id, stream_id),
        rep_release_entrys(entrys, j));

    uint64 pre_log_index = log_begin == CM_INVALID_INDEX_ID ? CM_INVALID_INDEX_ID : log_begin - 1;
    rep_init_appendlog_req(stream_id, appendlog_req, pre_log_index, last_log_index, j);

    if ((rep_encode_appendlog_req(&pack, appendlog_req) != CM_SUCCESS) || (mec_send_data(&pack) != CM_SUCCESS)) {
        rep_release_entrys(entrys, j);
        mec_release_pack(&pack);
        LOG_DEBUG_ERR("[REP]rep send append log failed: " REP_APPEND_REQ_FMT, REP_APPEND_REQ_VAL(&pack, appendlog_req));
        return CM_ERROR;
    }

    LOG_DEBUG_INF("[REP]rep send append log succeed: " REP_APPEND_REQ_FMT, REP_APPEND_REQ_VAL(&pack, appendlog_req));
    if (APPEND_MODE == APPEND_NORMAL_MODE) {
        (void)cm_atomic_cas((atomic_t*)&NEXT_INDEX, old_next_index, log_begin + j);
        LOG_DEBUG_INF("[REP]set next_index to %llu,stream_id=%u,node_id=%u", NEXT_INDEX, stream_id, node_id);
    }
    rep_release_entrys(entrys, j);
    mec_release_pack(&pack);
    return CM_SUCCESS;
}

static bool32 can_append_log(uint32 stream_id, uint64 last_index, uint32 node_id, dcf_role_t default_role)
{
    // only for passive node
    if (default_role == DCF_ROLE_PASSIVE && LOAD_LEVEL == DCF_LOAD_HIGH_LEVEL &&
        (g_timer()->now - PRE_APPTIME) < HIGH_LEVEL_SUSPEND_TIME) {
        return CM_FALSE;
    }

    // dn flow control, pause log replication
    if ((g_timer()->now - PRE_APPTIME) <= PAUSE_TIME) {
        return CM_FALSE;
    }

    /* if flow_ctrl=on, then do flow ctrl. */
    if (g_flow_ctrl_type != FC_NONE) {
        if ((g_timer()->now - PRE_APPTIME) < (g_rep_flow_ctrl_val * REP_FC_TIME_UNIT)) {
            return CM_FALSE;
        }
    }

    if ((APPEND_MODE == APPEND_NORMAL_MODE && last_index >= NEXT_INDEX) ||
        (APPEND_MODE == APPEND_REMATCH_MODE && cm_atomic32_cas(&TRY_REMATCH, 1, 0)) ||
        (g_timer()->now - PRE_APPTIME) > APPEND_INTERVAL*MICROSECS_PER_MILLISEC) {
        return CM_TRUE;
    }

    return CM_FALSE;
}

static status_t rep_appendlog_stream(uint64 thread_id, uint32 stream_id, uint32* stream_flag)
{
    dcf_node_role_t nodes[CM_MAX_NODE_COUNT];
    uint32 node_count;

    uint64 last_index = stg_last_index(stream_id);
    *stream_flag = 0;

    CM_RETURN_IFERR(md_get_stream_node_roles(stream_id, nodes, &node_count));

    for (uint32 i = 0; i < node_count; i++) {
        uint32 node_id = nodes[i].node_id;
        dcf_role_t default_role = nodes[i].default_role;
        if (node_id == g_cur_node_id) {
            continue;
        }

        if (thread_id != g_append_thread_id[i]) {
            continue;
        }

        if (!mec_is_ready(stream_id, node_id, PRIV_LOW)) {
            LOG_DEBUG_ERR_EX("[REP]stream_id%u, node_id%u's connection is not ready", stream_id, node_id);
            continue;
        }

        *stream_flag |= FLAG_EXISTS_ACTIVE_NODE;

        if (!can_append_log(stream_id, last_index, node_id, default_role)) {
            *stream_flag |= FLAG_CONTROL_FLOW;
            continue;
        }

        PRE_APPTIME = g_timer()->now;
        bool8 node_exists_log = CM_FALSE;

        if (rep_appendlog_node(stream_id, node_id, default_role, last_index, &node_exists_log) != CM_SUCCESS) {
            LOG_DEBUG_ERR("[REP]rep_appendlog_to_node failed:stream_id=%u,node_id=%u.", stream_id, node_id);
            continue;
        }

        if (node_exists_log) {
            *stream_flag |= FLAG_EXISTS_LOG;
        }
    }

    return CM_SUCCESS;
}

static void rep_appendlog_thread_entry(thread_t *thread)
{
    uint32 streams[CM_MAX_STREAM_COUNT];
    uint32 stream_count;
    uint64 thread_id = (uint64)thread->argument;
    uint32 rep_flag = 0;
    if (cm_set_thread_name("rep_appendlog") != CM_SUCCESS) {
        LOG_DEBUG_ERR("[REP]set apply thread name failed!");
    }

    if (md_get_stream_list(streams, &stream_count) != CM_SUCCESS) {
        LOG_DEBUG_ERR("[REP]md_get_stream_list failed");
        return;
    }

    while (!thread->closed) {
        rep_flag = 0;

        for (uint32 i = 0; i < stream_count; i++) {
            uint32 stream_id = streams[i];
            if (!I_AM_LEADER(stream_id)) {
                continue;
            }

            uint32 stream_flag = 0;
            if (rep_appendlog_stream(thread_id, stream_id, &stream_flag) != CM_SUCCESS) {
                LOG_DEBUG_ERR("[REP]rep_appendlog failed.");
                continue;
            }

            rep_flag |= stream_flag;
        }

        if (!(rep_flag & FLAG_EXISTS_ACTIVE_NODE)) {
            LOG_DEBUG_INF("[REP]not exists active node.");
            cm_sleep(CM_SLEEP_1000_FIXED);
            continue;
        }

        if (rep_flag & FLAG_CONTROL_FLOW) {
            (void)cm_wait_cond(&g_appendlog_cond, CM_SLEEP_1_FIXED);
            continue;
        }
        if (!(rep_flag & FLAG_EXISTS_LOG)) {
            (void)cm_wait_cond(&g_appendlog_cond, CM_SLEEP_500_FIXED);
        }
    }
}

void rep_flow_ctrl_sampling_and_calc()
{
    uint64 commit_count, commit_total, commit_max;
    static uint64 total_delay = 0;
    static uint64 last_total_delay = UINT64_MAX;
    static uint64 max_delay = 0;
    static uint64 min_delay = UINT64_MAX;
    int32 delta = 1;
    static int32 sleep_time = REP_FC_INIT_VAL;
    static int32 direction = 1;
    static uint64 count = 0;
    uint64 cur_delay = 0;

    static time_t last = 0;
    time_t now = time(NULL);
    if (now - last >= REP_FC_SAMP_PERIOD) {
        last = now;
        // use commit_delay as sampling value now, should classify by g_flow_ctrl_type if needed.
        ps_get_stat(PS_COMMIT, &commit_count, &commit_total, &commit_max);
        if (commit_count != 0) {
            cur_delay = commit_total / commit_count;
            total_delay += cur_delay;
            max_delay = MAX(max_delay, cur_delay);
            min_delay = MIN(min_delay, cur_delay);

            count++;
            if (count % REP_FC_CTRL_PERIOD == 0) {
                total_delay -= (max_delay + min_delay);
                if (sleep_time / CM_10X_FIXED > 1) {
                    delta = sleep_time / CM_10X_FIXED;
                }

                if (total_delay > last_total_delay) {
                    direction = 0 - direction;
                } else if (total_delay == last_total_delay) {
                    delta = 0;
                }
                last_total_delay = total_delay;

                sleep_time += delta * direction;
                sleep_time = MAX(sleep_time, 0);
                sleep_time = MIN(sleep_time, REP_FC_MAX_VAL);
                g_rep_flow_ctrl_val = (uint32)sleep_time;
                total_delay = 0;
                max_delay = 0;
                min_delay = UINT64_MAX;
            }
        }
        LOG_PROFILE("commit_count=%llu, mavg_delay=%llu, flow_ctrl_val=%u",
            commit_count, cur_delay, g_rep_flow_ctrl_val);
    }
}

static void rep_leader_monitor_entry(thread_t *thread)
{
    if (cm_set_thread_name("rep_leader_monitor") != CM_SUCCESS) {
        LOG_DEBUG_ERR("[REP]set monitor thread name failed!");
    }
    LOG_RUN_INF("leader monitor thread start.");
    while (!thread->closed) {
        if (g_flow_ctrl_type == FC_NONE) {
            cm_sleep(CM_SLEEP_1000_FIXED);
            continue;
        }
        rep_flow_ctrl_sampling_and_calc();
        (void)rep_monitor_statistics(&g_leader_monitor_statistics);
    }
    LOG_RUN_INF("leader monitor thread end.");
}

static int rep_index_compare(const void *a, const void *b)
{
    if ((*(uint64*)a == (*(uint64*)b))) {
        return 0;
    } else if ((*(uint64*)a > (*(uint64*)b))) {
        return -1;
    } else {
        return 1;
    }
}

static status_t rep_try_commit_log(uint32 stream_id)
{
    uint32 node_count, quorum;
    uint32 vote_count = 0;
    bool32 is_elc_voter;
    uint32 nodes[CM_MAX_NODE_COUNT];
    uint64 sort_index[CM_MAX_NODE_COUNT];
    uint64 min_apply_id = CM_INVALID_ID64;

    CM_RETURN_IFERR(elc_get_quorum(stream_id, &quorum));
    CM_RETURN_IFERR(md_get_stream_nodes(stream_id, nodes, &node_count));
    for (uint32 i = 0; i < node_count; i++) {
        uint32 node_id = nodes[i];
        CM_RETURN_IFERR(elc_is_voter(stream_id, node_id, &is_elc_voter));
        if (is_elc_voter) {
            uint64 index = MATCH_INDEX.index;
            sort_index[vote_count] = index;
            vote_count++;
            LOG_DEBUG_INF("[REP]rep_try_commit_log:node_id=%u,match_index=%llu.", node_id, index);
        }

        min_apply_id = MIN(min_apply_id, APPLY_INDEX);
    }

    if (quorum == 0 || quorum > vote_count) {
        LOG_RUN_ERR("[REP] invalid quorum:%u,vote_count;%u", quorum, vote_count);
        return CM_ERROR;
    }

    rep_set_cluster_min_apply_idx(stream_id, min_apply_id);
    qsort(sort_index, vote_count, sizeof(uint64), rep_index_compare);

    uint64 commit_index = sort_index[quorum - 1];
    uint64 log_term = stg_get_term(stream_id, commit_index);
    uint64 cur_term = elc_get_current_term(stream_id);
    LOG_DEBUG_INF("[REP]rep_try_commit_log:majority_count=%u,try commit_index=%llu,log_term=%llu,cur_term=%llu.",
        quorum, commit_index, log_term, cur_term);
    if (log_term == cur_term) {
        log_id_t last = rep_get_commit_log(stream_id);
        if (last.index != commit_index) {
            if (commit_index <= last.index) {
                LOG_DEBUG_WAR("[REP]current commit_index(%llu) is not larger than last.index(%llu), work_mode=%d",
                    commit_index, last.index, elc_get_work_mode(stream_id));
            }
            rep_set_commit_log(stream_id, log_term, commit_index);
            rep_apply_trigger();
            LOG_DEBUG_INF("[REP]leader set commit index to (%llu,%llu)", log_term, commit_index);
        }
    } else {
        LOG_DEBUG_INF("[REP]index term is not current term,can't be committed.index=%llu,"
            "log_term=%llu,current term = %llu", commit_index, log_term, cur_term);
    }

    return CM_SUCCESS;
}

status_t rep_leader_acceptlog_proc(uint32 stream_id)
{
    LOG_TRACE(rep_get_tracekey(), "accept:rep_leader_acceptlog_proc rep_try_commit_log.");
    LOG_DEBUG_INF("rep_leader_acceptlog_proc.");
    uint32 node_id = g_cur_node_id;
    APPLY_INDEX = stg_get_applied_index(stream_id);
    CM_RETURN_IFERR(rep_try_commit_log(stream_id));

    return CM_SUCCESS;
}

static void rep_rematch_proc(uint32 stream_id, uint32 node_id, rep_apendlog_ack_t* ack)
{
    APPEND_MODE = APPEND_REMATCH_MODE;
    log_id_t next_log = rep_get_pre_term_log(stream_id, ack->pre_log.index);
    LOG_DEBUG_INF("[REP] pre_log(%llu,%llu),mismatch_log(%llu,%llu),next_log(%llu,%llu)",
        ack->pre_log.term, ack->pre_log.index,
        ack->mismatch_log.term, ack->mismatch_log.index,
        next_log.term, next_log.index);

    if (next_log.index < ack->mismatch_log.index) {
        next_log = ack->mismatch_log;
    }

    if (NEXT_INDEX > next_log.index) {
        (void)cm_atomic_set((atomic_t*)&NEXT_INDEX, ack->mismatch_log.index);
        LOG_DEBUG_INF("[REP]pre log is mismatch,reset next index to:%llu,stream_id=%u,node_id=%u",
            NEXT_INDEX, stream_id, node_id);
    } else {
        LOG_DEBUG_INF("[REP]pre log is mismatch,next index:%llu,mismatch(%llu,%llu)",
            NEXT_INDEX, ack->mismatch_log.term, ack->mismatch_log.index);
    }
    (void)cm_atomic32_cas(&TRY_REMATCH, 0, 1);

    // retry to append log
    rep_appendlog_trigger(stream_id);
}

static status_t rep_check_appendlog_ack(uint32 stream_id, uint32 node_id, rep_apendlog_ack_t* ack)
{
    uint64 cur_term = elc_get_current_term(stream_id);
    if (ack->follower_term > cur_term) {
        // call election's function
        (void)elc_judge_term(stream_id, ack->follower_term);
        LOG_DEBUG_INF("[REP]follower's term is greater than mine.[%llu > %llu]", ack->follower_term, cur_term);
        return CM_ERROR;
    }

    if (ack->ret_code == ERR_TERM_IS_EXPIRED) {
        // call election's function
        (void)elc_judge_term(stream_id, ack->follower_term);
        LOG_DEBUG_INF("[REP]follower's term is greater than mine.[%llu,%llu]", ack->follower_term, cur_term);
        return CM_ERROR;
    } else if (ack->ret_code == ERR_APPEN_LOG_REQ_LOST) {
        LOG_DEBUG_INF("[REP]append log may be lost.reset next index from %llu to %llu,node_id=%u.",
            NEXT_INDEX, MATCH_INDEX.index + 1, node_id);
        NEXT_INDEX = MATCH_INDEX.index + 1;
    } else if (ack->ret_code == ERR_TERM_IS_NOT_MATCH) {
        rep_rematch_proc(stream_id, node_id, ack);
        return CM_ERROR;
    } else if (ack->ret_code != 0) {
        LOG_DEBUG_INF("[REP]follower process failed.ret_code=%d", ack->ret_code);
        return CM_ERROR;
    }

    return CM_SUCCESS;
}

// leader process follower's ack message
static status_t rep_appendlog_ack_proc(mec_message_t *pack)
{
    uint32 stream_id = pack->head->stream_id;
    uint32 node_id = pack->head->src_inst;
    rep_apendlog_ack_t ack;

    if (rep_decode_appendlog_ack(pack, &ack) != CM_SUCCESS) {
        LOG_DEBUG_ERR("[REP]rep_decode_appendlog_ack failed.");
        return CM_ERROR;
    }

    LOG_DEBUG_INF("[REP]recv ack." REP_APPEND_ACK_FMT, REP_APPEND_ACK_VAL(pack, &ack));

    if (ack.follower_accept_log.index != CM_INVALID_INDEX_ID) {
        ps_record1(PS_FOLLOWER_ACCEPT, ack.follower_accept_log.index);
    }

    CM_RETURN_IFERR(rep_check_appendlog_ack(stream_id, node_id, &ack));

    APPLY_INDEX = ack.apply_id;

    if (ack.follower_accept_log.index != CM_INVALID_INDEX_ID ||
        ack.follower_accept_log.term != CM_INVALID_TERM_ID) {
        uint64 my_term = stg_get_term(stream_id, ack.follower_accept_log.index);
        if (my_term == ack.follower_accept_log.term) {
            if (APPEND_MODE == APPEND_REMATCH_MODE) {
                APPEND_MODE = APPEND_NORMAL_MODE;
                (void)cm_atomic32_cas(&TRY_REMATCH, 1, 0);
                NEXT_INDEX = ack.follower_accept_log.index;
            }
            rep_follower_accepted_trigger(stream_id, pack->head->src_inst, ack.follower_accept_log);
            LOG_DEBUG_INF("[REP]follower process succeed,next_index=%llu,set match_index=(%llu,%llu)",
                NEXT_INDEX, MATCH_INDEX.term, MATCH_INDEX.index);
        } else {
            if (APPEND_MODE == APPEND_REMATCH_MODE) {
                APPEND_MODE = APPEND_NORMAL_MODE;
                (void)cm_atomic32_cas(&TRY_REMATCH, 1, 0);
                NEXT_INDEX = NEXT_INDEX + 1;
            }
        }
        log_id_t last_log = stg_last_log_id(stream_id);
        if (last_log.index >= NEXT_INDEX) {
            rep_appendlog_trigger(stream_id);
        }
    }

    return CM_SUCCESS;
}

void rep_appendlog_trigger(uint32 stream_id)
{
    LOG_DEBUG_INF("rep_appendlog_trigger.");
    cm_release_cond(&g_appendlog_cond);
}

static void rep_follower_accepted_trigger(uint32 stream_id, uint32 node_id, log_id_t log_id)
{
    LOG_TRACE(log_id.index, "rep_follower_accepted_trigger.");
    LOG_TRACE(rep_get_tracekey(), "rep_follower_accepted_trigger.log_id=%llu", log_id.index);
    LOG_DEBUG_INF("[REP]rep_follower_accepted_trigger,node_id=%u,log=(%llu,%llu)",
        node_id, log_id.term, log_id.index);

    MATCH_INDEX = log_id;

    rep_set_accept_flag(stream_id);
}

void rep_leader_acceptlog(uint32 stream_id, uint64 term, uint64 index, status_t status)
{
    if (status != CM_SUCCESS) {
        if (++DISK_ERROR >= DISK_ERROR_THRESHOLD) {
            DISK_ERROR = 0;
            (void)elc_demote_follower(stream_id);
        }
        return;
    }

    LOG_DEBUG_INF("rep_leader_acceptlog.");
    LOG_TRACE(index, "rep_leader_acceptlog.");

    uint32 node_id = g_cur_node_id;

    MATCH_INDEX.term = term;
    MATCH_INDEX.index = index;
    NEXT_INDEX = index + 1;
    DISK_ERROR = 0;
}

log_id_t rep_leader_get_match_index(uint32 stream_id, uint32 node_id)
{
    return MATCH_INDEX;
}

uint64 rep_leader_get_next_index(uint32 stream_id, uint32 node_id)
{
    return NEXT_INDEX;
}

uint64 rep_leader_get_apply_index(uint32 stream_id, uint32 node_id)
{
    return APPLY_INDEX;
}

void rep_set_pause_time(uint32 stream_id, uint32 node_id, uint32 pause_time)
{
    PAUSE_TIME = pause_time;
}

uint32 rep_get_pause_time(uint32 stream_id, uint32 node_id)
{
    return PAUSE_TIME;
}

static inline void rep_init_thread_id()
{
    uint64 node_id;
    uint64 node_cnt = 0;
    uint32 cur_node_id = md_get_cur_node();
    for (node_id = 0; node_id < CM_MAX_NODE_COUNT; node_id++) {
        g_append_thread_id[node_id] = node_cnt % g_append_thread_num;
        if (node_id != cur_node_id) {
            node_cnt++;
        }
    }
}
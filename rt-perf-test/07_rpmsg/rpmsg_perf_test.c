/*
 * RPMsg communication performance test for Spacemit K3 RCPU side.
 *
 * Linux sends framed packets to service "rpmsg:perf_test". RCPU echoes each
 * packet back immediately and periodically prints throughput/latency summary.
 *
 * Packet header is intentionally fixed-width and little-endian on K3 Linux/RCPU:
 *   magic      : 0x52504631 ("RPF1")
 *   seq        : packet sequence number
 *   send_ns    : Linux CLOCK_MONOTONIC timestamp in ns, used by Linux side
 *                to calculate round-trip latency after echo.
 *   payload_len: valid payload bytes after this header
 */

#include <openamp/remoteproc.h>
#include <openamp/rpmsg.h>
#include <openamp/rpmsg_virtio.h>
#include <openamp/virtio.h>
#include <rtdef.h>
#include <rtthread.h>
#include <string.h>

#define RPMSG_PERF_SERVICE_NAME       "rpmsg:perf_test"
#define RPMSG_PERF_ADDR_SRC           1010U
#define RPMSG_PERF_ADDR_DST           1011U
#define RPMSG_PERF_MAGIC              0x52504631U
#define RPMSG_PERF_HEADER_SIZE        20U
#define RPMSG_PERF_MAX_FRAME_SIZE     2048U
#define RPMSG_PERF_RECORD_INTERVAL    1000U
#define RPMSG_PERF_MAX_RECORDS        1024U
#define RPMSG_PERF_IDLE_TIMEOUT_MS    3000U
#define RPMSG_PERF_THREAD_STACK_SIZE  4096U

extern struct rpmsg_device *rpdev;

struct rpmsg_perf_frame {
    rt_uint32_t magic;
    rt_uint32_t seq;
    rt_uint64_t send_ns;
    rt_uint32_t payload_len;
    rt_uint8_t payload[RPMSG_PERF_MAX_FRAME_SIZE - RPMSG_PERF_HEADER_SIZE];
};

struct rpmsg_perf_record {
    rt_uint32_t packets;
    rt_uint32_t rx_bytes;
    rt_uint32_t tx_bytes;
    rt_uint32_t bad_packets;
    rt_uint32_t send_errors;
    rt_tick_t tick;
};

struct rpmsg_perf_ctx {
    char *service_name;
    struct rpmsg_endpoint endp;
    rt_bool_t endpoint_ready;
    volatile rt_bool_t test_running;
    volatile rt_bool_t summary_pending;
    volatile rt_uint32_t rx_packets;
    volatile rt_uint32_t tx_packets;
    volatile rt_uint32_t rx_bytes;
    volatile rt_uint32_t tx_bytes;
    volatile rt_uint32_t bad_packets;
    volatile rt_uint32_t send_errors;
    volatile rt_tick_t start_tick;
    volatile rt_tick_t last_rx_tick;
    volatile rt_uint32_t record_count;
    volatile rt_bool_t record_overflow;
    struct rpmsg_perf_record records[RPMSG_PERF_MAX_RECORDS];
};

static struct rpmsg_perf_ctx rpmsg_perf;

static rt_uint32_t rpmsg_perf_tick_to_ms(rt_tick_t tick)
{
    return (rt_uint32_t)(((rt_uint64_t)tick * 1000ULL) / RT_TICK_PER_SECOND);
}

static void rpmsg_perf_begin_session(void)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    rpmsg_perf.test_running = RT_TRUE;
    rpmsg_perf.summary_pending = RT_FALSE;
    rpmsg_perf.rx_packets = 0U;
    rpmsg_perf.tx_packets = 0U;
    rpmsg_perf.rx_bytes = 0U;
    rpmsg_perf.tx_bytes = 0U;
    rpmsg_perf.bad_packets = 0U;
    rpmsg_perf.send_errors = 0U;
    rpmsg_perf.record_count = 0U;
    rpmsg_perf.record_overflow = RT_FALSE;
    rpmsg_perf.start_tick = rt_tick_get();
    rpmsg_perf.last_rx_tick = rpmsg_perf.start_tick;
    rt_hw_interrupt_enable(level);
}

static void rpmsg_perf_record_snapshot(void)
{
    rt_uint32_t index;

    index = rpmsg_perf.record_count;
    if (index < RPMSG_PERF_MAX_RECORDS) {
        rpmsg_perf.records[index].packets = rpmsg_perf.rx_packets;
        rpmsg_perf.records[index].rx_bytes = rpmsg_perf.rx_bytes;
        rpmsg_perf.records[index].tx_bytes = rpmsg_perf.tx_bytes;
        rpmsg_perf.records[index].bad_packets = rpmsg_perf.bad_packets;
        rpmsg_perf.records[index].send_errors = rpmsg_perf.send_errors;
        rpmsg_perf.records[index].tick = rt_tick_get();
        rpmsg_perf.record_count = index + 1U;
    } else {
        rpmsg_perf.record_overflow = RT_TRUE;
    }
}

static int rpmsg_perf_endpoint_cb(struct rpmsg_endpoint *ept, void *data,
                                  size_t len, uint32_t src, void *priv)
{
    struct rpmsg_perf_frame *frame = (struct rpmsg_perf_frame *)data;
    int ret;

    (void)src;
    (void)priv;

    if (len < RPMSG_PERF_HEADER_SIZE || len > RPMSG_PERF_MAX_FRAME_SIZE ||
        frame->magic != RPMSG_PERF_MAGIC ||
        frame->payload_len + RPMSG_PERF_HEADER_SIZE > len) {
        rpmsg_perf.bad_packets++;
        return 0;
    }

    if (!rpmsg_perf.test_running) {
        rpmsg_perf_begin_session();
    }

    rpmsg_perf.rx_packets++;
    rpmsg_perf.rx_bytes += (rt_uint32_t)len;
    rpmsg_perf.last_rx_tick = rt_tick_get();

    ret = rpmsg_send(ept, data, len);
    if (ret < 0) {
        rpmsg_perf.send_errors++;
    } else {
        rpmsg_perf.tx_packets++;
        rpmsg_perf.tx_bytes += (rt_uint32_t)len;
    }

    if (rpmsg_perf.rx_packets % RPMSG_PERF_RECORD_INTERVAL == 0U) {
        rpmsg_perf_record_snapshot();
    }

    return 0;
}

static void rpmsg_perf_service_unbind(struct rpmsg_endpoint *ept)
{
    (void)ept;
    rpmsg_perf.endpoint_ready = RT_FALSE;
    rt_kprintf("[RPMSG_PERF] Service unbound\n");
}

static void rpmsg_perf_print_thread_entry(void *parameter)
{
    (void)parameter;

    while (1) {
        rt_thread_mdelay(200U);

        if (!rpmsg_perf.endpoint_ready) {
            continue;
        }

        if (rpmsg_perf.test_running &&
            (rt_tick_get() - rpmsg_perf.last_rx_tick) >=
            rt_tick_from_millisecond(RPMSG_PERF_IDLE_TIMEOUT_MS)) {
            if (rpmsg_perf.rx_packets > 0U &&
                (rpmsg_perf.rx_packets % RPMSG_PERF_RECORD_INTERVAL) != 0U) {
                rpmsg_perf_record_snapshot();
            }
            rpmsg_perf.test_running = RT_FALSE;
            rpmsg_perf.summary_pending = RT_TRUE;
        }

        if (rpmsg_perf.summary_pending) {
            rt_uint32_t i;
            rt_uint32_t prev_packets = 0U;
            rt_uint32_t prev_rx_bytes = 0U;
            rt_uint32_t prev_tx_bytes = 0U;
            rt_tick_t prev_tick = rpmsg_perf.start_tick;

            rt_kprintf("[RPMSG_PERF] Test idle %u ms, print recorded summary\n",
                       RPMSG_PERF_IDLE_TIMEOUT_MS);
            for (i = 0U; i < rpmsg_perf.record_count; ++i) {
                struct rpmsg_perf_record *record = &rpmsg_perf.records[i];
                rt_uint32_t delta_packets = record->packets - prev_packets;
                rt_uint32_t delta_rx_bytes = record->rx_bytes - prev_rx_bytes;
                rt_uint32_t delta_tx_bytes = record->tx_bytes - prev_tx_bytes;
                rt_uint32_t delta_ms = rpmsg_perf_tick_to_ms(record->tick - prev_tick);

                if (delta_ms == 0U) {
                    delta_ms = 1U;
                }

                rt_kprintf("[RPMSG_PERF] [%u] total=%u, delta=%u, rx=%u KB/s, tx=%u KB/s, bad=%u, err=%u\n",
                           i + 1U,
                           record->packets,
                           delta_packets,
                           delta_rx_bytes * 1000U / delta_ms / 1024U,
                           delta_tx_bytes * 1000U / delta_ms / 1024U,
                           record->bad_packets,
                           record->send_errors);

                prev_packets = record->packets;
                prev_rx_bytes = record->rx_bytes;
                prev_tx_bytes = record->tx_bytes;
                prev_tick = record->tick;
            }

            rt_kprintf("[RPMSG_PERF] Summary: rx=%u packets %u KB, tx=%u packets %u KB, bad=%u, err=%u%s\n",
                       rpmsg_perf.rx_packets,
                       rpmsg_perf.rx_bytes / 1024U,
                       rpmsg_perf.tx_packets,
                       rpmsg_perf.tx_bytes / 1024U,
                       rpmsg_perf.bad_packets,
                       rpmsg_perf.send_errors,
                       rpmsg_perf.record_overflow ? ", record_overflow" : "");

            rpmsg_perf.summary_pending = RT_FALSE;
        }
    }
}

static void rpmsg_perf_init_thread_entry(void *parameter)
{
    int ret;

    (void)parameter;

    while (rpdev == RT_NULL) {
        rt_thread_delay(10);
    }

    rt_kprintf("[RPMSG_PERF] rpdev ready, creating endpoint...\n");

    rpmsg_perf.service_name = RPMSG_PERF_SERVICE_NAME;
    ret = rpmsg_create_ept(&rpmsg_perf.endp, rpdev, rpmsg_perf.service_name,
                           RPMSG_PERF_ADDR_SRC, RPMSG_PERF_ADDR_DST,
                           rpmsg_perf_endpoint_cb, rpmsg_perf_service_unbind);
    if (ret) {
        rt_kprintf("[RPMSG_PERF] Create endpoint failed, ret=%d\n", ret);
        return;
    }

    rpmsg_perf.endpoint_ready = RT_TRUE;
    rt_kprintf("[RPMSG_PERF] Endpoint created: %s (src=%u, dst=%u), max_frame=%u, tx_payload_limit=%d, rx_payload_limit=%d\n",
               rpmsg_perf.service_name,
               RPMSG_PERF_ADDR_SRC,
               RPMSG_PERF_ADDR_DST,
               RPMSG_PERF_MAX_FRAME_SIZE,
               rpmsg_virtio_get_tx_buffer_size(rpdev),
               rpmsg_virtio_get_rx_buffer_size(rpdev));
}

int rpmsg_perf_start(void)
{
    rt_thread_t init_tid;
    rt_thread_t print_tid;

    if (rpmsg_perf.endpoint_ready) {
        rt_kprintf("[RPMSG_PERF] Already started\n");
        return 0;
    }

    rt_memset(&rpmsg_perf, 0, sizeof(rpmsg_perf));

    init_tid = rt_thread_create("rpmsg_pi", rpmsg_perf_init_thread_entry,
                                RT_NULL, RPMSG_PERF_THREAD_STACK_SIZE,
                                RT_THREAD_PRIORITY_MAX / 3, 20);
    if (init_tid == RT_NULL) {
        rt_kprintf("[RPMSG_PERF] Failed to create init thread\n");
        return -RT_EINVAL;
    }
    rt_thread_startup(init_tid);

    print_tid = rt_thread_create("rpmsg_ps", rpmsg_perf_print_thread_entry,
                                 RT_NULL, RPMSG_PERF_THREAD_STACK_SIZE,
                                 RT_THREAD_PRIORITY_MAX / 3 + 1, 20);
    if (print_tid == RT_NULL) {
        rt_kprintf("[RPMSG_PERF] Failed to create stat thread\n");
        return -RT_EINVAL;
    }
    rt_thread_startup(print_tid);

    rt_kprintf("[RPMSG_PERF] Service starting...\n");
    return 0;
}

static int cmd_rpmsg_perf(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return rpmsg_perf_start();
}

#include <finsh.h>
MSH_CMD_EXPORT_ALIAS(cmd_rpmsg_perf, rpmsg_perf, RPMsg communication performance echo test);

/*
 * This program aims at creating a customized full blown DPDK application
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_common.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_timer.h>
#include <rte_cycles.h>
#include <rte_ring.h>
#include <rte_malloc.h>
#include <rte_string_fns.h>

/* Set of macros */
#define MBUF_CACHE_SIZE 512
#define RX_RINGS 2
#define RX_RING_SIZE 4096
#define PORT_ID 2
#define PORT_MASK 0x4
#define BURST_SIZE 2048
#define MAX_RX_QUEUE_PER_PORT 128
#define MAX_RX_DESC 4096
#define RX_RING_SZ 65536
#define FLOW_NUM 65536
#define WRITE_FILE
#define MAX_LCORE_PARAMS 1024

#define IPG

/* mask of enabled ports. */
uint32_t enabled_port_mask = 0x4;

/* number of rx queues, 2 by default. */
uint8_t nb_rxq = RX_RINGS;

/* number of rx ring descriptors, 4096 by default. */
uint16_t nb_rx_desc = RX_RING_SIZE;

/* batch size for packet fetch */
uint16_t burst_size = BURST_SIZE;

/* Set in promiscuous mode on by default. */
static unsigned promiscuous_on = 1;

uint16_t n_rx_thread, n_tx_thread;

static struct rte_ring *rings[RX_RINGS];

static struct rte_timer timer;

struct rte_eth_rxconf rx_conf;

uint64_t gCtr[RX_RINGS];

#ifdef IPG
static uint64_t global = 0;
#endif

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = ETHER_MAX_LEN,
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 0, /**< IP checksum offload enabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame    = 1, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_PROTO_MASK,
		}
	},
};

enum {
	CMD_LINE_OPT_NB_RXQ_NUM = 256,
	CMD_LINE_OPT_NB_RX_DESC,
	CMD_LINE_OPT_BURST_SIZE,
	CMD_LINE_OPT_CONFIG,
	CMD_LINE_OPT_RX_CONFIG,
	CMD_LINE_OPT_TX_CONFIG
};

struct rx_thread_params {
	uint8_t port_id;
	uint8_t queue_id;
	uint8_t lcore_id;
	uint8_t thread_id;
}__rte_cache_aligned;

static struct rx_thread_params rx_thread_params_array[MAX_LCORE_PARAMS];
static struct rx_thread_params rx_thread_params_array_default[] = {
	{2, 0, 1, 0},
	{2, 1, 2, 1},
};

static struct rx_thread_params *rx_thread_params =
		rx_thread_params_array_default;
static uint16_t nb_rx_thread_params = RTE_DIM(rx_thread_params_array_default);

struct tx_thread_params {
	uint8_t lcore_id;
	uint8_t thread_id;
} __rte_cache_aligned;

static struct tx_thread_params tx_thread_params_array[MAX_LCORE_PARAMS];
static struct tx_thread_params tx_thread_params_array_default[] = {
	{3, 0},
	{4, 1},
};

static struct tx_thread_params *tx_thread_params =
		tx_thread_params_array_default;
static uint16_t nb_tx_thread_params = RTE_DIM(tx_thread_params_array_default);

struct pkt_count
{
	uint16_t hi_f1;
	uint16_t hi_f2;
	uint32_t ctr[2];

	#ifdef IPG
        uint64_t ipg[2];
	double  avg[2], stdDev[2];

	#ifdef QUANTILE
        struct quantile qt[2];
        #endif

        #endif

	//struct flow_entry *flows;

}__rte_cache_aligned;

static struct pkt_count pkt_ctr[FLOW_NUM]__rte_cache_aligned;

static void timer_cb(__attribute__((unused)) struct rte_timer *tim,
			__attribute__((unused)) void *arg)
{
	uint8_t i;
	double j = 0;
//	static double old = 0;

	struct rte_eth_stats eth_stats;
	rte_eth_stats_get(PORT_ID, &eth_stats);

	struct timespec tstamp;

	// test the timestamping ability, may got removed in the future.
	if (!rte_eth_timesync_read_rx_timestamp(2, &tstamp, 0))
		puts("Timestamp detected...");

	for(i=0; i<RX_RINGS; i++)
		j += gCtr[i];

//	printf("RX rate: %.2lf Mpps, Total RX pkts: %.0lf, Total dropped pkts: %lu\n",
//						 (j - old)/1000000, j, eth_stats.imissed);

	printf("RX: Total RX pkts: %.0lf, Total dropped pkts: %lu\n", j, eth_stats.imissed);
/*	old = j;

	#ifdef IPG
		#ifdef LINKED_LIST
			printf("[IPG] Average IPG: %.0lf\n", flows[65246]->avg);
		#endif
		#ifdef DOUBLE_HASH
			printf("[IPG] Average IPG: %.0lf\n", pkt_ctr[65246].avg[0]);
		#endif
		#ifdef HASH_LIST
			printf("[IPG] Average IPG: %.0lf, stdDev %lf\n", pkt_ctr[65246].avg[0], pkt_ctr[65246].stdDev[0]);
		#endif
	#endif
*/
}

/* display usage */
static void print_usage(const char *prgname)
{
	printf("%s [EAL options] --\n"\
	        " -p  PORTMASK: hexadecimal bitmask of ports to configure\n"\
		" -P: enable promiscuous mode\n"
	        " --nb_rxq: Rx queues\n"\
	        " --nb_rx_desc: The size of RX descriptors\n"\
		" --burst_size: The reception batch size\n"\
		" [--rx (port,queue,lcore,thread)[,(port,queue,lcore,thread]]\n"
		" [--tx (lcore,thread)[,(lcore,thread]]\n",
	       prgname);
}

static int parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}

static int parse_rx_queue_nb(const char *rxq)
{
	char *end = NULL;
	unsigned long pm;

	pm = strtoul(rxq, &end, 10);
	if ((rxq[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	if (pm > MAX_RX_QUEUE_PER_PORT)
		pm = MAX_RX_QUEUE_PER_PORT;

	return pm;
}

static int parse_rx_desc_nb(const char *rxdesc)
{
	char *end = NULL;
	unsigned long pm;

	pm = strtoul(rxdesc, &end, 10);
	if ((rxdesc[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	if (pm > MAX_RX_DESC)
		pm = MAX_RX_DESC;

	return pm;
}


static int parse_rx_burst_size(const char *burst)
{
	char *end = NULL;
	unsigned long pm;

	pm = strtoul(burst, &end, 10);
	if ((burst[0] == '\0') || (end == NULL) || (*end != '\0'))
                return -1;

	if (pm == 0)
                return -1;

	return pm;
}

static int
parse_rx_config(const char *q_arg)
{
	char s[256];
	const char *p, *p0 = q_arg;
	char *end;
	enum fieldnames
	{
		FLD_PORT = 0,
		FLD_QUEUE,
		FLD_LCORE,
		FLD_THREAD,
		_NUM_FLD
	};
	unsigned long int_fld[_NUM_FLD];
	char *str_fld[_NUM_FLD];
	int i;
	unsigned size;

	nb_rx_thread_params = 0;

	while ((p = strchr(p0, '(')) != NULL)
	{
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		snprintf(s, sizeof(s), "%.*s", size, p);
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') != _NUM_FLD)
			return -1;

		for (i = 0; i < _NUM_FLD; i++)
		{
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] || int_fld[i] > 255)
				return -1;
		}

		if (nb_rx_thread_params >= MAX_LCORE_PARAMS)
		{
			printf("exceeded max number of rx params: %hu\n",
					nb_rx_thread_params);
			return -1;
		}

		rx_thread_params_array[nb_rx_thread_params].port_id =
				(uint8_t)int_fld[FLD_PORT];
		rx_thread_params_array[nb_rx_thread_params].queue_id =
				(uint8_t)int_fld[FLD_QUEUE];
		rx_thread_params_array[nb_rx_thread_params].lcore_id =
				(uint8_t)int_fld[FLD_LCORE];
		rx_thread_params_array[nb_rx_thread_params].thread_id =
				(uint8_t)int_fld[FLD_THREAD];
		++nb_rx_thread_params;
	}
	rx_thread_params = rx_thread_params_array;
	return 0;
}

static int
parse_tx_config(const char *q_arg)
{
	char s[256];
	const char *p, *p0 = q_arg;
	char *end;
	enum fieldnames {
		FLD_LCORE = 0,
		FLD_THREAD,
		_NUM_FLD
	};
	unsigned long int_fld[_NUM_FLD];
	char *str_fld[_NUM_FLD];
	int i;
	unsigned size;

	nb_tx_thread_params = 0;

	while ((p = strchr(p0, '(')) != NULL) {
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		snprintf(s, sizeof(s), "%.*s", size, p);
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') != _NUM_FLD)
			return -1;
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] || int_fld[i] > 255)
				return -1;
		}
		if (nb_tx_thread_params >= MAX_LCORE_PARAMS) {
			printf("exceeded max number of tx params: %hu\n",
				nb_tx_thread_params);
			return -1;
		}
		tx_thread_params_array[nb_tx_thread_params].lcore_id =
				(uint8_t)int_fld[FLD_LCORE];
		tx_thread_params_array[nb_tx_thread_params].thread_id =
				(uint8_t)int_fld[FLD_THREAD];
		++nb_tx_thread_params;
	}
	tx_thread_params = tx_thread_params_array;

	return 0;
}

/* parse the application arguments. */
static int parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	char *prgname = argv[0];

	static struct option lgopts[] = {
		{"nb_rxq", 1, 0, CMD_LINE_OPT_NB_RXQ_NUM},
		{"nb_rx_desc", 1, 0, CMD_LINE_OPT_NB_RX_DESC},
		{"burst_size", 1, 0,  CMD_LINE_OPT_BURST_SIZE},
		{"rx_conf", 1, 0, CMD_LINE_OPT_RX_CONFIG},
		{"tx_conf", 1, 0, CMD_LINE_OPT_TX_CONFIG},
		{NULL, 0, 0, 0}
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "p:P",
				lgopts, NULL)) != EOF)
	{
		switch (opt)
		{
			case 'p':
				enabled_port_mask = parse_portmask(optarg);
				if (enabled_port_mask == 0)
				{
					printf("Invalid port mask\n");
					print_usage(prgname);
					return -1;
				}
				printf("enabled port mask is %x\n", enabled_port_mask);
				break;

			case 'P':
				printf("Promiscuous mode enabled\n");
				promiscuous_on = 1;
				break;

			case CMD_LINE_OPT_NB_RXQ_NUM:
				ret = parse_rx_queue_nb(optarg);
				if (ret != -1)
				{
					nb_rxq = ret;
					printf("The number of rxq is %d\n", nb_rxq);
				}
				break;

			case CMD_LINE_OPT_NB_RX_DESC:
				ret = parse_rx_desc_nb(optarg);
				if (ret != -1)
				{
					nb_rx_desc = ret;
					printf("The number of RX descriptors is %d\n", nb_rx_desc);
				}
				break;

			case CMD_LINE_OPT_BURST_SIZE:
				ret = parse_rx_burst_size(optarg);
				if (ret != -1)
				{
					burst_size = ret;
					printf("The packet reception batch size is %d\n", burst_size);
				}
				break;

			case CMD_LINE_OPT_RX_CONFIG:
				ret = parse_rx_config(optarg);
				if (ret)
				{
					printf("invalid rx-config\n");
					print_usage(prgname);
					return -1;
				}
				break;

			case CMD_LINE_OPT_TX_CONFIG:
				ret = parse_tx_config(optarg);
				if (ret)
				{
					printf("invalid tx-config\n");
                                        print_usage(prgname);
                                        return -1;
				}
				break;

			default:
				return -1;
				print_usage(prgname);
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind - 1;
	optind = 1;
	return ret;
}

/*
static int
init_rx_queues(void)
{
	uint16_t i, nb_rx_queue;
	uint8_t thread;

	n_rx_thread = 0;

	for (i = 0; i < nb_rx_thread_params; ++i) {
		thread = rx_thread_params[i].thread_id;
		nb_rx_queue = rx_thread[thread].n_rx_queue;

		if (nb_rx_queue >= MAX_RX_QUEUE_PER_LCORE) {
			printf("error: too many queues (%u) for thread: %u\n",
				(unsigned)nb_rx_queue + 1, (unsigned)thread);
			return -1;
		}

		rx_thread[thread].conf.thread_id = thread;
		rx_thread[thread].conf.lcore_id = rx_thread_params[i].lcore_id;
		rx_thread[thread].rx_queue_list[nb_rx_queue].port_id =
			rx_thread_params[i].port_id;
		rx_thread[thread].rx_queue_list[nb_rx_queue].queue_id =
			rx_thread_params[i].queue_id;
		rx_thread[thread].n_rx_queue++;

		if (thread >= n_rx_thread)
			n_rx_thread = thread + 1;

	}
	return 0;
}

static int
init_tx_threads(void)
{
	int i;

	n_tx_thread = 0;
	for (i = 0; i < nb_tx_thread_params; ++i) {
		tx_thread[n_tx_thread].conf.thread_id = tx_thread_params[i].thread_id;
		tx_thread[n_tx_thread].conf.lcore_id = tx_thread_params[i].lcore_id;
		n_tx_thread++;
	}
	return 0;
}

*/

/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port
 */
static inline void
lcore_main_rx(__attribute__((unused)) void *dummy)
{
	uint8_t port;
	uint32_t buf;
	int q;

	unsigned lcore_id;
	lcore_id = rte_lcore_id();
	//q = lcore_conf[lcore_id].queue_id;

	printf("Setting: lcore %u checks queue %d\n", lcore_id, q);

	/* Run until the application is quit or killed. */
	for (;;)
	{
		port = PORT_ID;
		struct rte_mbuf *bufs[burst_size];

		const uint16_t nb_rx = rte_eth_rx_burst(port, q,
				bufs, burst_size);

		if (unlikely(nb_rx == 0))
			continue;

		rte_ring_enqueue_burst(rings[q],
                                (void *)bufs, nb_rx, NULL);

		/* Per packet processing */
		for (buf = 0; buf < nb_rx; buf++)
			rte_pktmbuf_free(bufs[buf]);
	}
}

/*
 * The lcore main. This is the main thread that does the per-flow statistics
 */
static inline void
lcore_main_px(__attribute__((unused)) void *dummy)
{
	unsigned lcore_id = rte_lcore_id(), q;
	uint16_t nb_rx, index_l, index_h;
	uint32_t buf;
	struct rte_mbuf *bufs[burst_size];

	if (lcore_id == 4)
		q = 0;
	else if (lcore_id == 2)
		q = 1;

	printf("Settings: lcore %d dequeues queue %d\n", lcore_id, q);
	for(;;)
	{
		//struct rte_mbuf *bufs[burst_size];
		nb_rx = rte_ring_dequeue_burst(rings[q], (void *)bufs, burst_size, NULL);
		if (unlikely(nb_rx == 0))
                        continue;
		gCtr[q] += nb_rx;

		/* Per packet processing */
                for (buf = 0; buf < nb_rx; buf++)
                {
			index_l = bufs[buf]->hash.rss & 0xffff;
			index_h = (bufs[buf]->hash.rss & 0xffff0000)>>16;

			rte_pktmbuf_free(bufs[buf]);
			if(pkt_ctr[index_l].hi_f1 == 0)
			{
				pkt_ctr[index_l].hi_f1 = index_h;
				pkt_ctr[index_l].ctr[0]++;

				#ifdef IPG
				pkt_ctr[index_l].avg[0] = pkt_ctr[index_l].ipg[0];
				#endif
			}
			else if(pkt_ctr[index_l].hi_f2 == 0 && pkt_ctr[index_l].hi_f1 != index_h)
			{
				pkt_ctr[index_l].hi_f2 = index_h;
				pkt_ctr[index_l].ctr[1]++;

				#ifdef IPG
				pkt_ctr[index_l].avg[1] = pkt_ctr[index_l].ipg[1];
				#endif
			}
			else
			{
				if(pkt_ctr[index_l].hi_f1 == index_h)
				{
					pkt_ctr[index_l].ctr[0]++;

					#ifdef IPG
					pkt_ctr[index_l].avg[0] =
						((pkt_ctr[index_l].avg[0] * (pkt_ctr[index_l].ctr[0] - 1)) + (global - 1 - pkt_ctr[index_l].ipg[0]))/(float)pkt_ctr[index_l].ctr[0];

					pkt_ctr[index_l].ipg[0] = global;
					#endif
				}
				else if(pkt_ctr[index_l].hi_f2 == index_h)
				{
					pkt_ctr[index_l].ctr[1]++;

					#ifdef IPG
					pkt_ctr[index_l].avg[1] =
						(pkt_ctr[index_l].avg[1] * (pkt_ctr[index_l].ctr[1] - 1) + global -
							 1 - pkt_ctr[index_l].ipg[1])/(float)pkt_ctr[index_l].ctr[1];

					pkt_ctr[index_l].ipg[1] = global;
					#endif
				}
			}
                }
	}
}

static void handler(int sig)
{
	int portid, i;

	printf("\nSignal %d received\n", sig);

        for (portid = 0; portid < rte_eth_dev_count(); portid++)
        {
                if ((enabled_port_mask & (1 << portid)) == 0)
                        continue;

		printf("statistics for port %d:\n", portid);

		struct rte_eth_stats eth_stats;
		rte_eth_stats_get(portid, &eth_stats);

		puts("Stoping the device..\n");
	        rte_eth_dev_stop(portid);
	        printf("[DPDK]  Received pkts %lu \n\tDropped packets %lu \n\tErroneous packets %lu\n",
				eth_stats.ipackets + eth_stats.imissed + eth_stats.ierrors,
				eth_stats.imissed, eth_stats.ierrors);

		uint64_t sum = 0;
 	        for(i=0; i<RX_RINGS; i++)
 	        {
			sum += gCtr[i];
	                printf("\nQueue %d counter's value: %lu\n", i, gCtr[i]);
			//rte_ring_free(rings[i]);
	        }
		printf("\nValue of global counter: %lu\n", sum);

		sum = 0;
		for(i=0; i<FLOW_NUM; i++)
		{
			//sum += pkt_ctr[i].ctr[0] < 100?0:1;
			sum += pkt_ctr[i].ctr[1]?1:0 + pkt_ctr[i].ctr[0]?1:0;

/*			if (pkt_ctr[i].ctr[0]!=0)
				printf("flow %d: %u, %u\n", i, pkt_ctr[i].hi_f1, pkt_ctr[i].ctr[0]);
			if (pkt_ctr[i].ctr[1]!=0)
                                printf("%u\n", pkt_ctr[i].hi_f2, pkt_ctr[i].ctr[1]);
*/		}
		printf("\nThe total number of flows is %lu\n", sum);

	#ifdef WRITE_FILE
	FILE *fp;
	fp = fopen("./tmp.txt", "a");
	fprintf(fp, "%lu %lu %lu %lu %lu\n", eth_stats.ipackets + eth_stats.imissed + eth_stats.ierrors, eth_stats.imissed, gCtr[0], gCtr[1], sum);
	fclose(fp);
	#endif

	}
	exit(1);
}

int main(int argc, char **argv)
{
	struct rte_mempool *mbuf_pool;
	unsigned lcore_id;
	uint32_t nb_lcores, nb_ports;
	uint8_t portid, qid;

	signal(SIGINT, handler);

	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	/* parse the application arguments */
	printf("The configuration of %s\n", argv[0]);
	ret = parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Wrong APP parameters\n");

	//ret = parse_lcore_conf();
	//if (ret < 0)
	//	rte_exit(EXIT_FAILURE, "The numbers of RX queues and lcores do not match\n");

	rte_timer_subsystem_init();

	rte_timer_init(&timer);

	uint64_t hz = rte_get_timer_hz();
	lcore_id = rte_lcore_id();
	rte_timer_reset(&timer, hz, PERIODICAL, lcore_id, timer_cb, NULL);

	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", 65535,
			MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

        rx_conf.rx_thresh.pthresh = 8;
        rx_conf.rx_thresh.hthresh = 8;
        rx_conf.rx_thresh.wthresh = 0;
	rx_conf.rx_free_thresh = 2048;
        rx_conf.rx_drop_en = 0;

	nb_lcores = rte_lcore_count();
	printf("The number of lcores is %u\n", nb_lcores);

	nb_ports = rte_eth_dev_count();
	printf("The number of ports is %u\n", nb_ports);

	for (portid = 0; portid < nb_ports; portid++)
	{
		if ((enabled_port_mask & (1 << portid)) == 0)
		{
			printf("\nSkip disabled port %d\n", portid);
			continue;
		}

		/* init ports */
		printf("\nInitialize port %d ...\n", portid);
		fflush(stdout);

		ret = rte_eth_dev_configure(portid, (uint16_t)nb_rxq, 0, &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d,"
					"port=%d, rxq=%d\n", ret, portid, nb_rxq);

		for (qid = 0; qid < nb_rxq; qid++)
		{
			ret = rte_eth_rx_queue_setup(portid, qid, nb_rx_desc,
					rte_eth_dev_socket_id(portid), &rx_conf, mbuf_pool);
			if (ret < 0)
				rte_exit(EXIT_FAILURE, "Cannot setup queue %d\n", qid);

			char ring_name[256];
			snprintf(ring_name, sizeof(ring_name), "transient_ring_%d", qid);
			rings[qid] = rte_ring_create(ring_name, RX_RING_SZ,
                        	rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
			if (rings[qid] == NULL)
				rte_exit(EXIT_FAILURE, "Cannot create transient ring\n");
		}

		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot start port %d\n", portid);

		/* Display the port MAC address. */
		struct ether_addr addr;
		rte_eth_macaddr_get(portid, &addr);
		printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
				   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
				(unsigned)portid,
				addr.addr_bytes[0], addr.addr_bytes[1],
				addr.addr_bytes[2], addr.addr_bytes[3],
				addr.addr_bytes[4], addr.addr_bytes[5]);

		/* Enable RX in promiscuous mode for the Ethernet device. */
		if (promiscuous_on == 1)
			rte_eth_promiscuous_enable(portid);
	}

	int i;
	for (i=0; i<FLOW_NUM; i++)
	{
		pkt_ctr[i].ctr[0] = pkt_ctr[i].ctr[1] = 0;
	}

/*	RTE_LCORE_FOREACH_SLAVE(lcore_id)
	{
		if (lcore_id == 9 || lcore_id == 6)
			rte_eal_remote_launch((lcore_function_t *)lcore_main_rx, NULL, lcore_id);
		else
			rte_eal_remote_launch((lcore_function_t *)lcore_main_px, NULL, lcore_id);
	}
*/

	rte_eal_mp_remote_launch(pthread_run, NULL, SKIP_MASTER);
	while(1)
		rte_timer_manage();

	return EXIT_SUCCESS;
}
#include <getopt.h>

#include "dpdk.h"

#include <string>
#include <rte_ring.h>
#include <rte_eth_ring.h>

DpdkRte* DpdkRte::rte_ = nullptr;
std::mutex DpdkRte::mutex_;

DpdkRte::~DpdkRte()
{
    ClosePorts();
}

DpdkRte* DpdkRte::Instance()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (rte_ == nullptr) {
        rte_ = new DpdkRte;
    }
    return rte_;
}

#define RING_SIZE 256
#define NUM_RINGS 2
#define SOCKET0 0

struct rte_ring *ring[NUM_RINGS];
int port0, port1;

int DpdkRte::RteInit(int argc, char *argv[], bool secondry)
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
    cap_core_num_ = 1;
    core_num = rte_lcore_count();
    port_num = rte_eth_dev_count();

    if (port_num == 0 && !secondry) {
        ring[0] = rte_ring_create("R0", RING_SIZE, SOCKET0, RING_F_SP_ENQ|RING_F_SC_DEQ);
        ring[1] = rte_ring_create("R1", RING_SIZE, SOCKET0, RING_F_SP_ENQ|RING_F_SC_DEQ);
/* create two ethdev's */
        port0 = rte_eth_from_rings("net_ring0", ring, NUM_RINGS, ring, NUM_RINGS, SOCKET0);
        port1 = rte_eth_from_rings("net_ring1", ring, NUM_RINGS, ring, NUM_RINGS, SOCKET0);
        port_num += 2;
    }


    mbuf_size_ = 0;
    argc -= ret;
    argv += ret;

    int opt;
    while ((opt = getopt(argc, argv, "b:c:")) != -1) {
       switch (opt) {
       case 'b':
           mbuf_size_ = atoi(optarg);
           break;      
       case 'c':
           cap_core_num_ = atoi(optarg);
           break;
       default: /* '?' */
           rte_exit(EXIT_FAILURE, "Invalid EAL parameters\n");
       }
    }
    return ret;
}

int DpdkRte::PortsInit()
{
    char pool_name[256] = {0};
    for (int i = 0; i < port_num; i++) {
        DpdkPort* port = new DpdkPort(i, cap_core_num_, 0);
        ports_.push_back(port);
        uint8_t socketid = port->SocketId();
        if (ports_mempools_.count(socketid) == 0) {
            //how to calculate the total  memory ??
            unsigned n = (port->RxRings() * port->RxDesc() + port->TxRings() * port->TxDesc() + MBUF_CACHE_SIZE + 1024)
                         * cap_core_num_ * 16;
            n += mbuf_size_;
            printf("pktmbuf_pool size = %u, mbuf_size_=%d, cap_core_num_= %d\n", n, mbuf_size_, cap_core_num_);
            snprintf(pool_name, sizeof pool_name, "%s_%d", "DpdkRte_MBUF_POOL", i);
            RteMemPoolPtr mbuf_pool = rte_pktmbuf_pool_create(pool_name, 
                                                              n,
                                                              MBUF_CACHE_SIZE, 
                                                              0, 
                                                              RTE_MBUF_DEFAULT_BUF_SIZE, 
                                                              socketid);
            if (mbuf_pool == NULL) {
                rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
            }
            ports_mempools_[socketid] = mbuf_pool;
        }
    }

    for (std::vector<DpdkPort*>::iterator x = ports_.begin(); x != ports_.end(); x++) {
        int rt = (*x)->Setup(ports_mempools_[(*x)->SocketId()]);
        if (rt != 0) {
            rte_exit(EXIT_FAILURE, "fail to Setup \n");
        }
    }
    return 0;
}

void DpdkRte::StartPorts()
{
    for (std::vector<DpdkPort*>::iterator x = ports_.begin(); x != ports_.end(); x++) {
        (*x)->Start();
    }
}

void DpdkRte::StopPorts()
{
    for (std::vector<DpdkPort*>::iterator x = ports_.begin(); x != ports_.end(); x++) {
        (*x)->Stop();
    }
}

void DpdkRte::ClosePorts()
{
    for (std::vector<DpdkPort*>::iterator x = ports_.begin(); x != ports_.end(); x++) {
        (*x)->Close();
    }
}

void DpdkRte::PrintInfo()
{
    printf("core_num = %d\n", core_num);
    printf("port_num = %d\n", port_num);
}

DpdkPort::DpdkPort(uint8_t port_id, uint16_t rx_rings, uint16_t tx_rings)
    : port_id_(port_id),
      rx_rings_(rx_rings),
      tx_rings_(tx_rings),
      num_rxdesc_(4096),
      num_txdesc_(4096)
{
    char dev_name[128];
    port_conf_.rxmode.mq_mode = ETH_MQ_RX_RSS;
    port_conf_.rx_adv_conf.rss_conf.rss_key = nullptr; // use defaut DPDK-key
    //port_conf_.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IP | ETH_RSS_TCP | ETH_RSS_UDP;
    port_conf_.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_PROTO_MASK;
    // Tune tx
    port_conf_.txmode.mq_mode = ETH_MQ_TX_NONE;

    if (rte_eth_dev_is_valid_port(port_id_) == 0) {
        rte_exit(EXIT_FAILURE, "rte_eth_dev_is_valid_port \n");
    }

    if (rte_eth_dev_get_name_by_port(port_id_, dev_name) != 0) {
        rte_exit(EXIT_FAILURE, "fail to get port name \n");
    }
    dev_name_ = std::string(dev_name);

    socket_id_ = rte_eth_dev_socket_id(port_id_);
    core_id_  = rte_lcore_id();
    rte_eth_dev_info_get(1, &dev_info_);
    printf("port_id = %d, socket_id_ = %d\n", port_id, socket_id_);
}

DpdkPort::~DpdkPort()
{

}

int DpdkPort::Setup(RteMemPoolPtr mbuf_pool)
{
    int ret = 0;
    // if (rx_rings_ > 1) {
    //     port_conf_.rxmode.mq_mode = ETH_MQ_RX_RSS;
    //     port_conf_.rx_adv_conf.rss_conf.rss_key = nullptr;
    //     port_conf_.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_PROTO_MASK;
    // }
    ret = rte_eth_dev_configure(port_id_, rx_rings_, tx_rings_, &port_conf_);
    if (ret) {
        return ret;
    }

    for (int q = 0; q < rx_rings_; q++) {
        ret = rte_eth_rx_queue_setup(port_id_, q, num_rxdesc_, socket_id_, nullptr, mbuf_pool);
        if (ret) {
            return ret;
        }
    }

    for (int q = 0; q < tx_rings_; q++) {
        ret = rte_eth_tx_queue_setup(port_id_, q, num_txdesc_, socket_id_, nullptr);
        if (ret) {
            return ret;
        }
    }

    rte_eth_promiscuous_enable(port_id_);
    return 0;
}

int DpdkPort::Start()
{
    rte_eth_dev_stop(port_id_);
    int ret = rte_eth_dev_start(port_id_);
    if (ret) {
        rte_exit(EXIT_FAILURE, "Cannot start port %d \n", port_id_);
    }
    return 0;
}

void DpdkPort::Stop()
{
    rte_eth_dev_stop(port_id_);
}

void DpdkPort::Close()
{
    rte_eth_dev_stop(port_id_);
    rte_eth_dev_close(port_id_);
}
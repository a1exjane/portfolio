// SPDX-License-Identifier: GPL-2.0-only
/*
 * virt_net.c — Виртуальный сетевой интерфейс с поддержкой ping
 *
 * Создаёт "vnet0". IP задаётся через /proc/sys/net/virt_net/ip_addr.
 * Назначение адреса запускается через workqueue → call_usermodehelper,
 * что позволяет вызвать /sbin/ip из ядра без exported-символов devinet.
 *
 * Быстрый старт:
 *   insmod virt_net.ko
 *   echo "10.0.0.2/24" > /proc/sys/net/virt_net/ip_addr
 *   sleep 1 && ping 10.0.0.2
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/sysctl.h>
#include <linux/inet.h>
#include <linux/inetdevice.h>
#include <linux/workqueue.h>
#include <linux/umh.h>
#include <net/checksum.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kernel Module Test");
MODULE_DESCRIPTION("Virtual NIC: ICMP ping responder, IP via sysctl + workqueue");
MODULE_VERSION("4.0");

#define DRV_NAME "vnet0"

/* ──────────────────────────────────────────────────────────────────── */
/*  Глобальное состояние                                                */
/* ──────────────────────────────────────────────────────────────────── */

static struct net_device       *g_dev;
static struct ctl_table_header *g_sysctl_hdr;

/* Строка для sysctl */
static char  g_ip_str[32]   = "не задан";

/* Target IP — на него отвечаем на ICMP Echo Request */
static __be32 g_target_ip   = 0;
static __be32 g_target_mask = 0;

/* ──────────────────────────────────────────────────────────────────── */
/*  Workqueue: назначение IP через call_usermodehelper                  */
/*                                                                      */
/*  call_usermodehelper требует process context (нельзя из atomic или   */
/*  из некоторых syscall-обработчиков). Workqueue (kworker) — это       */
/*  именно process context, поэтому запуск userspace-команды отсюда     */
/*  гарантированно работает.                                            */
/* ──────────────────────────────────────────────────────────────────── */

/* Буфер для shell-команды (защищён единственным воркером) */
static char g_shell_cmd[256];

/* Воркер: выполняется в контексте kworker-потока */
static void ip_assign_worker(struct work_struct *work);
static DECLARE_WORK(g_ip_work, ip_assign_worker);

/* Переменные окружения для дочернего процесса */
static char * const g_envp[] = {
    "HOME=/",
    "PATH=/usr/sbin:/sbin:/usr/bin:/bin",
    NULL
};

static void ip_assign_worker(struct work_struct *work)
{
    char *argv[] = { "/bin/sh", "-c", g_shell_cmd, NULL };
    int   ret;

    pr_info(DRV_NAME ": выполняем: %s\n", g_shell_cmd);

    ret = call_usermodehelper("/bin/sh", argv,
                              (char **)g_envp, UMH_WAIT_PROC);
    if (ret)
        /*
         * Ненулевой ret = (exit_code << 8) | signal.
         * Например, если ip вернул 1 → ret = 256.
         * Это не фатально — пользователь увидит в dmesg.
         */
        pr_warn(DRV_NAME ": команда завершилась с кодом %d "
                "(exit=%d)\n", ret, ret >> 8);
    else
        pr_info(DRV_NAME ": IP успешно назначен\n");
}

/* ──────────────────────────────────────────────────────────────────── */
/*  Парсинг строки и планирование назначения IP                        */
/* ──────────────────────────────────────────────────────────────────── */

static int parse_and_apply(const char *s)
{
    char   buf[32];
    char  *slash;
    __be32 ip, mask, local_ip;
    int    prefix = 24;
    int    len;

    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Убираем пробелы/переводы строки */
    len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' ||
                        buf[len-1] == ' '))
        buf[--len] = '\0';

    slash = strchr(buf, '/');
    if (slash) {
        *slash++ = '\0';
        if (kstrtoint(slash, 10, &prefix) || prefix < 1 || prefix > 32)
            return -EINVAL;
    }

    ip = in_aton(buf);
    if (!ip)
        return -EINVAL;

    mask = (prefix < 32) ? htonl(~0u << (32 - prefix)) : htonl(~0u);

    /*
     * Локальный IP интерфейса: та же подсеть, host-часть = 1.
     * Нужен чтобы ядро создало маршрут подсети через vnet0.
     * Пример: target 10.0.0.2/24 → local 10.0.0.1/24
     * Если совпадает с target → host-часть = 2.
     */
    local_ip = htonl((ntohl(ip) & ntohl(mask)) | 1);
    if (local_ip == ip)
        local_ip = htonl((ntohl(ip) & ntohl(mask)) | 2);

    g_target_ip   = ip;
    g_target_mask = mask;

    pr_info(DRV_NAME ": target=%pI4  local=%pI4  /%d\n",
            &g_target_ip, &local_ip, prefix);

    /*
     * Формируем команду: поднимаем интерфейс, удаляем старые адреса,
     * назначаем новый. Всё в одной строке для /bin/sh -c.
     */
    snprintf(g_shell_cmd, sizeof(g_shell_cmd),
             "ip link set %s up && "
             "ip addr flush dev %s && "
             "ip addr add %pI4/%d dev %s",
             DRV_NAME, DRV_NAME, &local_ip, prefix, DRV_NAME);

    /*
     * Планируем выполнение в воркере. schedule_work возвращает
     * немедленно — sysctl-запись не блокируется.
     */
    schedule_work(&g_ip_work);

    return 0;
}

/* ──────────────────────────────────────────────────────────────────── */
/*  sysctl: /proc/sys/net/virt_net/ip_addr                             */
/* ──────────────────────────────────────────────────────────────────── */

static int ip_sysctl_handler(struct ctl_table *table, int write,
                              void *buffer, size_t *lenp, loff_t *ppos)
{
    char saved[32];
    int  ret;

    if (write)
        strncpy(saved, g_ip_str, sizeof(saved));

    ret = proc_dostring(table, write, buffer, lenp, ppos);
    if (ret || !write)
        return ret;

    ret = parse_and_apply(g_ip_str);
    if (ret) {
        strncpy(g_ip_str, saved, sizeof(g_ip_str));
        pr_err(DRV_NAME ": неверный формат, нужно: A.B.C.D или A.B.C.D/N\n");
    }
    return ret;
}

static struct ctl_table g_sysctl_table[] = {
    {
        .procname     = "ip_addr",
        .data         = g_ip_str,
        .maxlen       = sizeof(g_ip_str),
        .mode         = 0644,
        .proc_handler = ip_sysctl_handler,
    },
    { }
};

/* ──────────────────────────────────────────────────────────────────── */
/*  ICMP Echo Reply                                                     */
/* ──────────────────────────────────────────────────────────────────── */

static void build_and_inject_reply(struct sk_buff *req)
{
    struct ethhdr  *eth_req = eth_hdr(req);
    struct iphdr   *iph_req = ip_hdr(req);
    struct icmphdr *ich_req = (struct icmphdr *)((u8 *)iph_req +
                                                  iph_req->ihl * 4);
    u16 icmp_len = ntohs(iph_req->tot_len) - iph_req->ihl * 4;
    struct sk_buff *rep;
    struct ethhdr  *eth_rep;
    struct iphdr   *iph_rep;
    struct icmphdr *ich_rep;

    rep = dev_alloc_skb(ETH_HLEN + sizeof(*iph_rep) + icmp_len + NET_IP_ALIGN);
    if (unlikely(!rep))
        return;
    skb_reserve(rep, NET_IP_ALIGN);

    /* Ethernet: меняем src↔dst MAC */
    eth_rep = skb_put(rep, ETH_HLEN);
    ether_addr_copy(eth_rep->h_dest,   eth_req->h_source);
    ether_addr_copy(eth_rep->h_source, eth_req->h_dest);
    eth_rep->h_proto = htons(ETH_P_IP);

    /* IP: меняем src↔dst, пересчитываем checksum */
    iph_rep        = skb_put(rep, sizeof(*iph_rep));
    *iph_rep       = *iph_req;
    iph_rep->saddr = iph_req->daddr;
    iph_rep->daddr = iph_req->saddr;
    iph_rep->ttl   = 64;
    iph_rep->check = 0;
    iph_rep->check = ip_fast_csum((u8 *)iph_rep, iph_rep->ihl);

    /* ICMP: тип 8 → 0 (Echo Reply), пересчитываем checksum */
    ich_rep           = skb_put(rep, icmp_len);
    memcpy(ich_rep, ich_req, icmp_len);
    ich_rep->type     = ICMP_ECHOREPLY;
    ich_rep->checksum = 0;
    ich_rep->checksum = ip_compute_csum(ich_rep, icmp_len);

    /* Инъектируем в RX-стек */
    rep->dev       = g_dev;
    skb_reset_mac_header(rep);
    rep->protocol  = eth_type_trans(rep, g_dev);
    rep->ip_summed = CHECKSUM_NONE;

    if (netif_rx(rep) == NET_RX_SUCCESS) {
        g_dev->stats.rx_packets++;
        g_dev->stats.rx_bytes += sizeof(*iph_rep) + icmp_len;
    } else {
        g_dev->stats.rx_dropped++;
    }
}

/* ──────────────────────────────────────────────────────────────────── */
/*  net_device_ops                                                      */
/* ──────────────────────────────────────────────────────────────────── */

static int vnet_open(struct net_device *dev)
{
    netif_start_queue(dev);
    return 0;
}

static int vnet_stop(struct net_device *dev)
{
    netif_stop_queue(dev);
    return 0;
}

static netdev_tx_t vnet_start_xmit(struct sk_buff *skb,
                                    struct net_device *dev)
{
    struct iphdr   *iph;
    struct icmphdr *ich;

    dev->stats.tx_packets++;
    dev->stats.tx_bytes += skb->len;

    if (skb->protocol != htons(ETH_P_IP))
        goto drop;

    if (!pskb_may_pull(skb, ETH_HLEN + sizeof(struct iphdr)))
        goto drop;

    iph = ip_hdr(skb);

    if (!g_target_ip || iph->daddr != g_target_ip)
        goto drop;

    if (iph->protocol != IPPROTO_ICMP)
        goto drop;

    if (!pskb_may_pull(skb, ETH_HLEN + iph->ihl * 4 + sizeof(struct icmphdr)))
        goto drop;

    /* После pskb_may_pull указатели могут измениться */
    iph = ip_hdr(skb);
    ich = (struct icmphdr *)((u8 *)iph + iph->ihl * 4);

    if (ich->type != ICMP_ECHO || ich->code != 0)
        goto drop;

    pr_debug(DRV_NAME ": ping %pI4→%pI4 seq=%u\n",
             &iph->saddr, &iph->daddr, ntohs(ich->un.echo.sequence));

    build_and_inject_reply(skb);
drop:
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

static const struct net_device_ops vnet_ops = {
    .ndo_open       = vnet_open,
    .ndo_stop       = vnet_stop,
    .ndo_start_xmit = vnet_start_xmit,
};

static void vnet_setup(struct net_device *dev)
{
    ether_setup(dev);
    dev->netdev_ops = &vnet_ops;
    dev->flags     |= IFF_NOARP;
    dev->flags     &= ~IFF_MULTICAST;
    eth_hw_addr_random(dev);
}

/* ──────────────────────────────────────────────────────────────────── */
/*  init / exit                                                         */
/* ──────────────────────────────────────────────────────────────────── */

static int __init vnet_init(void)
{
    int ret;

    g_dev = alloc_netdev(0, DRV_NAME, NET_NAME_PREDICTABLE, vnet_setup);
    if (!g_dev)
        return -ENOMEM;

    ret = register_netdev(g_dev);
    if (ret) {
        pr_err(DRV_NAME ": register_netdev = %d\n", ret);
        free_netdev(g_dev);
        return ret;
    }

    g_sysctl_hdr = register_net_sysctl(&init_net, "net/virt_net",
                                        g_sysctl_table);
    if (!g_sysctl_hdr) {
        pr_err(DRV_NAME ": register_net_sysctl провалился\n");
        unregister_netdev(g_dev);
        free_netdev(g_dev);
        return -ENOMEM;
    }

    pr_info(DRV_NAME ": загружен.\n"
            "  Задайте IP: echo \"10.0.0.2/24\" > /proc/sys/net/virt_net/ip_addr\n"
            "  Подождите ~1 сек, затем: ping 10.0.0.2\n");
    return 0;
}

static void __exit vnet_exit(void)
{
    /* Ждём завершения текущего воркера перед выгрузкой */
    cancel_work_sync(&g_ip_work);

    unregister_net_sysctl_table(g_sysctl_hdr);
    unregister_netdev(g_dev);
    free_netdev(g_dev);
    pr_info(DRV_NAME ": выгружен\n");
}

module_init(vnet_init);
module_exit(vnet_exit);
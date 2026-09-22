// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP32-S31 Gigabit Ethernet IPC netdev
 *
 * The RGMII MAC and YT8531 PHY stay with ESP-IDF on hart 0.  Linux exchanges
 * Ethernet frames through shared internal SRAM, mirroring the WLAN modem
 * driver.
 */

#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>

#include "esp32s31-eth-ipc.h"

#define ESP32S31_ETH_NAPI_WEIGHT		16
#define ESP32S31_ETH_DOORBELL_RX		0x0
#define ESP32S31_ETH_DOORBELL_TX		0x4

struct esp32s31_eth {
	struct net_device *ndev;
	struct napi_struct napi;
	struct esp32s31_eth_ipc __iomem *ipc;
	void __iomem *doorbell;
};

static struct esp32s31_eth *esp32s31_eth_priv(struct net_device *ndev)
{
	return *(struct esp32s31_eth **)netdev_priv(ndev);
}

static struct esp32s31_eth_ipc_slot __iomem *
esp32s31_eth_slot(struct esp32s31_eth_ipc_ring __iomem *ring, u32 index)
{
	return &ring->slot[index % ESP32S31_ETH_IPC_SLOTS];
}

static bool esp32s31_eth_tx_full(struct esp32s31_eth_ipc_ring __iomem *ring)
{
	u32 head = ioread32(&ring->head);
	u32 tail = ioread32(&ring->tail);

	return head - tail >= ESP32S31_ETH_IPC_SLOTS;
}

static void esp32s31_eth_sync_carrier(struct esp32s31_eth *priv)
{
	if (ioread32(&priv->ipc->link_up))
		netif_carrier_on(priv->ndev);
	else
		netif_carrier_off(priv->ndev);
}

static int esp32s31_eth_poll(struct napi_struct *napi, int budget)
{
	struct esp32s31_eth *priv = container_of(napi, struct esp32s31_eth, napi);
	struct esp32s31_eth_ipc_ring __iomem *ring = &priv->ipc->to_linux;
	int done = 0;

	while (done < budget) {
		struct esp32s31_eth_ipc_slot __iomem *slot;
		struct sk_buff *skb;
		u32 tail = ioread32(&ring->tail);
		u32 head = ioread32(&ring->head);
		u32 len;

		if (tail == head)
			break;

		slot = esp32s31_eth_slot(ring, tail);
		len = ioread32(&slot->len);
		if (len == 0 || len > ESP32S31_ETH_IPC_SLOT_DATA) {
			iowrite32(tail + 1, &ring->tail);
			priv->ndev->stats.rx_errors++;
			continue;
		}

		skb = napi_alloc_skb(napi, len);
		if (!skb) {
			priv->ndev->stats.rx_dropped++;
			break;
		}

		memcpy_fromio(skb_put(skb, len), slot->data, len);
		iowrite32(tail + 1, &ring->tail);
		skb->protocol = eth_type_trans(skb, priv->ndev);
		napi_gro_receive(napi, skb);
		priv->ndev->stats.rx_packets++;
		priv->ndev->stats.rx_bytes += len;
		done++;
	}

	esp32s31_eth_sync_carrier(priv);

	if (netif_queue_stopped(priv->ndev) &&
	    !esp32s31_eth_tx_full(&priv->ipc->to_firmware))
		netif_wake_queue(priv->ndev);

	if (done < budget)
		napi_complete_done(napi, done);

	return done;
}

static irqreturn_t esp32s31_eth_irq(int irq, void *dev_id)
{
	struct esp32s31_eth *priv = dev_id;

	iowrite32(0, priv->doorbell + ESP32S31_ETH_DOORBELL_RX);
	napi_schedule(&priv->napi);
	return IRQ_HANDLED;
}

static netdev_tx_t esp32s31_eth_xmit(struct sk_buff *skb,
				     struct net_device *ndev)
{
	struct esp32s31_eth *priv = esp32s31_eth_priv(ndev);
	struct esp32s31_eth_ipc_ring __iomem *ring = &priv->ipc->to_firmware;
	struct esp32s31_eth_ipc_slot __iomem *slot;
	u32 head = ioread32(&ring->head);
	unsigned int len = skb->len;

	if (len > ESP32S31_ETH_IPC_SLOT_DATA) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	if (esp32s31_eth_tx_full(ring)) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	slot = esp32s31_eth_slot(ring, head);
	memcpy_toio(slot->data, skb->data, len);
	iowrite32(len, &slot->len);
	iowrite32(head + 1, &ring->head);
	iowrite32(1, priv->doorbell + ESP32S31_ETH_DOORBELL_TX);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	dev_kfree_skb_any(skb);

	if (esp32s31_eth_tx_full(ring)) {
		netif_stop_queue(ndev);
		if (!esp32s31_eth_tx_full(ring))
			netif_wake_queue(ndev);
	}

	return NETDEV_TX_OK;
}

static int esp32s31_eth_open(struct net_device *ndev)
{
	struct esp32s31_eth *priv = esp32s31_eth_priv(ndev);

	napi_enable(&priv->napi);
	esp32s31_eth_sync_carrier(priv);
	netif_start_queue(ndev);
	return 0;
}

static int esp32s31_eth_stop(struct net_device *ndev)
{
	struct esp32s31_eth *priv = esp32s31_eth_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	napi_disable(&priv->napi);
	return 0;
}

static const struct net_device_ops esp32s31_eth_netdev_ops = {
	.ndo_open = esp32s31_eth_open,
	.ndo_stop = esp32s31_eth_stop,
	.ndo_start_xmit = esp32s31_eth_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static int esp32s31_eth_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct esp32s31_eth *priv;
	struct net_device *ndev;
	u8 mac[ETH_ALEN];
	u32 magic;
	int irq, ret, i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ipc = devm_platform_ioremap_resource_byname(pdev, "ipc");
	if (IS_ERR(priv->ipc))
		return PTR_ERR(priv->ipc);

	priv->doorbell = devm_platform_ioremap_resource_byname(pdev, "doorbell");
	if (IS_ERR(priv->doorbell))
		return PTR_ERR(priv->doorbell);

	for (i = 0; i < 50; i++) {
		magic = ioread32(&priv->ipc->magic);
		if (magic == ESP32S31_ETH_IPC_MAGIC)
			break;
		msleep(20);
	}
	if (magic != ESP32S31_ETH_IPC_MAGIC) {
		dev_err(dev, "ethernet IPC not ready (magic 0x%08x)\n", magic);
		return -ENODEV;
	}
	if (ioread32(&priv->ipc->version) != ESP32S31_ETH_IPC_VERSION) {
		dev_err(dev, "unsupported ethernet IPC version %u\n",
			ioread32(&priv->ipc->version));
		return -EINVAL;
	}

	ndev = alloc_etherdev(sizeof(struct esp32s31_eth *));
	if (!ndev)
		return -ENOMEM;

	*(struct esp32s31_eth **)netdev_priv(ndev) = priv;
	priv->ndev = ndev;
	SET_NETDEV_DEV(ndev, dev);
	ndev->netdev_ops = &esp32s31_eth_netdev_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = ESP32S31_ETH_IPC_SLOT_DATA - ETH_HLEN;

	for (i = 0; i < ETH_ALEN; i++)
		mac[i] = ioread8(&priv->ipc->mac[i]);
	eth_hw_addr_set(ndev, mac);

	netif_napi_add(ndev, &priv->napi, esp32s31_eth_poll);
	platform_set_drvdata(pdev, priv);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_free;
	}

	ret = devm_request_irq(dev, irq, esp32s31_eth_irq, 0,
			       dev_name(dev), priv);
	if (ret)
		goto err_free;

	ret = register_netdev(ndev);
	if (ret)
		goto err_free;

	esp32s31_eth_sync_carrier(priv);
	dev_info(dev, "eth0 via hart-0 EMAC IPC at 0x%08x\n",
		 ESP32S31_ETH_IPC_SRAM_ADDR);
	return 0;

err_free:
	netif_napi_del(&priv->napi);
	free_netdev(ndev);
	return ret;
}

static void esp32s31_eth_remove(struct platform_device *pdev)
{
	struct esp32s31_eth *priv = platform_get_drvdata(pdev);

	unregister_netdev(priv->ndev);
	netif_napi_del(&priv->napi);
	free_netdev(priv->ndev);
}

static const struct of_device_id esp32s31_eth_of_match[] = {
	{ .compatible = "esp,esp32s31-eth" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_eth_of_match);

static struct platform_driver esp32s31_eth_driver = {
	.probe = esp32s31_eth_probe,
	.remove = esp32s31_eth_remove,
	.driver = {
		.name = "esp32s31-eth",
		.of_match_table = esp32s31_eth_of_match,
	},
};
module_platform_driver(esp32s31_eth_driver);

MODULE_DESCRIPTION("ESP32-S31 Gigabit Ethernet IPC netdev");
MODULE_LICENSE("GPL");

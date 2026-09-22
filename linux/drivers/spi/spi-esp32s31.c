// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Mueller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 GPSPI master (CPU FIFO mode)
 *
 * SPI2 is left for the loader's ILI9341; this driver binds GPSPI3.  Transfers
 * go through the 64-byte CPU buffer -- GDMA is not wired yet.  Pads are
 * claimed through the GPIO matrix using the SoC pad numbers in DT.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define ESP32S31_SPI_FIFO_BYTES		64
#define ESP32S31_SPI_FIFO_WORDS		(ESP32S31_SPI_FIFO_BYTES / 4)

/* Relative to the SPI register window. */
#define ESP32S31_SPI_CMD		0x00
#define ESP32S31_SPI_USR			BIT(24)
#define ESP32S31_SPI_UPDATE		BIT(23)
#define ESP32S31_SPI_CTRL		0x08
#define ESP32S31_SPI_CLOCK		0x0c
#define ESP32S31_SPI_CLKCNT_L		GENMASK(5, 0)
#define ESP32S31_SPI_CLKCNT_H		GENMASK(11, 6)
#define ESP32S31_SPI_CLKCNT_N		GENMASK(17, 12)
#define ESP32S31_SPI_CLKDIV_PRE		GENMASK(21, 18)
#define ESP32S31_SPI_CLK_EQU_SYSCLK	BIT(31)
#define ESP32S31_SPI_USER		0x10
#define ESP32S31_SPI_DOUTDIN		BIT(0)
#define ESP32S31_SPI_CK_OUT_EDGE	BIT(9)
#define ESP32S31_SPI_USR_MOSI		BIT(27)
#define ESP32S31_SPI_USR_MISO		BIT(28)
#define ESP32S31_SPI_USER1		0x14
#define ESP32S31_SPI_USER2		0x18
#define ESP32S31_SPI_MS_DLEN		0x1c
#define ESP32S31_SPI_MS_DATA_BITLEN	GENMASK(17, 0)
#define ESP32S31_SPI_MISC		0x20
#define ESP32S31_SPI_CS0_DIS		BIT(0)
#define ESP32S31_SPI_CS1_DIS		BIT(1)
#define ESP32S31_SPI_CS2_DIS		BIT(2)
#define ESP32S31_SPI_CS3_DIS		BIT(3)
#define ESP32S31_SPI_CS4_DIS		BIT(4)
#define ESP32S31_SPI_CS5_DIS		BIT(5)
#define ESP32S31_SPI_CK_IDLE_EDGE	BIT(29)
#define ESP32S31_SPI_DMA_CONF		0x30
#define ESP32S31_SPI_RX_AFIFO_RST	BIT(29)
#define ESP32S31_SPI_BUF_AFIFO_RST	BIT(30)
#define ESP32S31_SPI_DMA_INT_CLR		0x38
#define ESP32S31_SPI_DMA_INT_RAW		0x3c
#define ESP32S31_SPI_TRANS_DONE		BIT(12)
#define ESP32S31_SPI_W0			0x98
#define ESP32S31_SPI_SLAVE		0xe0
#define ESP32S31_SPI_SOFT_RESET		BIT(27)

/* GPSPI3_CTRL0 in HP_SYS_CLKRST. */
#define ESP32S31_SPI_CLK_SYS_EN		BIT(0)
#define ESP32S31_SPI_CLK_APB_EN		BIT(1)
#define ESP32S31_SPI_CLK_RST_EN		BIT(2)
#define ESP32S31_SPI_CLK_SRC_SEL	GENMASK(5, 4)
#define ESP32S31_SPI_CLK_SRC_XTAL	0
#define ESP32S31_SPI_CLK_HS_EN		BIT(6)
#define ESP32S31_SPI_CLK_HS_DIV		GENMASK(14, 7)
#define ESP32S31_SPI_CLK_MST_DIV	GENMASK(22, 15)
#define ESP32S31_SPI_CLK_MST_EN		BIT(23)

/* GPIO matrix / IO MUX, same layout as gpio-esp32s31. */
#define ESP32S31_GPIO_ENABLE		0x34
#define ESP32S31_GPIO_ENABLE_W1TS	0x38
#define ESP32S31_GPIO_ENABLE_W1TC	0x3c
#define ESP32S31_GPIO_ENABLE1		0x40
#define ESP32S31_GPIO_ENABLE1_W1TS	0x44
#define ESP32S31_GPIO_ENABLE1_W1TC	0x48
#define ESP32S31_GPIO_FUNC_IN_SEL	0x2f4
#define ESP32S31_GPIO_SIG_IN_SEL	BIT(9)
#define ESP32S31_GPIO_FUNC_OUT_SEL	0xaf4
#define ESP32S31_IOMUX_FUN_IE		BIT(9)
#define ESP32S31_IOMUX_MCU_SEL		GENMASK(14, 12)
#define ESP32S31_IOMUX_FUNC_GPIO	1

/* GPSPI3 pad signals (soc/gpio_sig_map.h). */
#define ESP32S31_SPI3_CK_OUT		47
#define ESP32S31_SPI3_Q_IN		48
#define ESP32S31_SPI3_D_OUT		49

#define ESP32S31_SPI_CLKDIV_PRE_MAX	16
#define ESP32S31_SPI_DONE_TIMEOUT_US	100000

struct esp32s31_spi {
	struct spi_controller *host;
	void __iomem *regs;
	void __iomem *clkrst;
	void __iomem *gpio;
	void __iomem *iomux;
	struct clk *clk;
	u32 clk_hz;
	u32 cur_speed_hz;
	u8 cur_mode;
	u8 sck_pin;
	u8 mosi_pin;
	u8 miso_pin;
};

static void esp32s31_spi_write(struct esp32s31_spi *esp, u32 off, u32 val)
{
	writel(val, esp->regs + off);
}

static u32 esp32s31_spi_read(struct esp32s31_spi *esp, u32 off)
{
	return readl(esp->regs + off);
}

static void esp32s31_spi_iomux_gpio(struct esp32s31_spi *esp, unsigned int pin)
{
	void __iomem *iomux = esp->iomux + pin * 4;
	u32 val = readl(iomux);

	val &= ~ESP32S31_IOMUX_MCU_SEL;
	val |= FIELD_PREP(ESP32S31_IOMUX_MCU_SEL, ESP32S31_IOMUX_FUNC_GPIO);
	val |= ESP32S31_IOMUX_FUN_IE;
	writel(val, iomux);
}

static void esp32s31_spi_enable_out(struct esp32s31_spi *esp, unsigned int pin)
{
	unsigned int reg = pin < 32 ? ESP32S31_GPIO_ENABLE_W1TS :
				      ESP32S31_GPIO_ENABLE1_W1TS;

	writel(BIT(pin % 32), esp->gpio + reg);
}

static void esp32s31_spi_disable_out(struct esp32s31_spi *esp, unsigned int pin)
{
	unsigned int reg = pin < 32 ? ESP32S31_GPIO_ENABLE_W1TC :
				      ESP32S31_GPIO_ENABLE1_W1TC;

	writel(BIT(pin % 32), esp->gpio + reg);
}

static void esp32s31_spi_matrix_out(struct esp32s31_spi *esp, unsigned int pin,
				    u32 signal)
{
	esp32s31_spi_iomux_gpio(esp, pin);
	/* Peripheral drives OE (OE_SEL = 0). */
	writel(signal, esp->gpio + ESP32S31_GPIO_FUNC_OUT_SEL + pin * 4);
	esp32s31_spi_enable_out(esp, pin);
}

static void esp32s31_spi_matrix_in(struct esp32s31_spi *esp, unsigned int pin,
				   u32 signal)
{
	esp32s31_spi_iomux_gpio(esp, pin);
	esp32s31_spi_disable_out(esp, pin);
	writel(pin | ESP32S31_GPIO_SIG_IN_SEL,
	       esp->gpio + ESP32S31_GPIO_FUNC_IN_SEL + signal * 4);
}

static void esp32s31_spi_setup_pins(struct esp32s31_spi *esp)
{
	esp32s31_spi_matrix_out(esp, esp->sck_pin, ESP32S31_SPI3_CK_OUT);
	esp32s31_spi_matrix_out(esp, esp->mosi_pin, ESP32S31_SPI3_D_OUT);
	esp32s31_spi_matrix_in(esp, esp->miso_pin, ESP32S31_SPI3_Q_IN);
}

static void esp32s31_spi_enable_clock(struct esp32s31_spi *esp)
{
	u32 ctrl;

	/*
	 * XTAL into the GPSPI functional clock with no pre-divide.  The
	 * peripheral then divides further in SPI_CLOCK for the pad SCLK.
	 */
	ctrl = ESP32S31_SPI_CLK_SYS_EN | ESP32S31_SPI_CLK_APB_EN |
	       ESP32S31_SPI_CLK_HS_EN | ESP32S31_SPI_CLK_MST_EN |
	       FIELD_PREP(ESP32S31_SPI_CLK_SRC_SEL, ESP32S31_SPI_CLK_SRC_XTAL) |
	       FIELD_PREP(ESP32S31_SPI_CLK_HS_DIV, 0) |
	       FIELD_PREP(ESP32S31_SPI_CLK_MST_DIV, 0);
	writel(ctrl | ESP32S31_SPI_CLK_RST_EN, esp->clkrst);
	writel(ctrl, esp->clkrst);
}

static void esp32s31_spi_hw_init(struct esp32s31_spi *esp)
{
	esp32s31_spi_write(esp, ESP32S31_SPI_SLAVE, ESP32S31_SPI_SOFT_RESET);
	esp32s31_spi_write(esp, ESP32S31_SPI_SLAVE, 0);
	esp32s31_spi_write(esp, ESP32S31_SPI_USER, 0);
	esp32s31_spi_write(esp, ESP32S31_SPI_USER1, 0);
	esp32s31_spi_write(esp, ESP32S31_SPI_USER2, 0);
	esp32s31_spi_write(esp, ESP32S31_SPI_CTRL, 0);
	esp32s31_spi_write(esp, ESP32S31_SPI_DMA_CONF, 0);
	/* CS comes from cs-gpios; keep every hardware CS idle. */
	esp32s31_spi_write(esp, ESP32S31_SPI_MISC,
			   ESP32S31_SPI_CS0_DIS | ESP32S31_SPI_CS1_DIS |
			   ESP32S31_SPI_CS2_DIS | ESP32S31_SPI_CS3_DIS |
			   ESP32S31_SPI_CS4_DIS | ESP32S31_SPI_CS5_DIS);
	esp32s31_spi_write(esp, ESP32S31_SPI_DMA_INT_CLR,
			   ESP32S31_SPI_TRANS_DONE);
}

static u32 esp32s31_spi_cal_clock(u32 src_hz, u32 hz, u32 *reg_out)
{
	u32 best_pre = 1, best_n = 2, best_err = U32_MAX;
	u32 pre, n, eff, err, reg;

	if (hz >= (src_hz / 4) * 3) {
		*reg_out = ESP32S31_SPI_CLK_EQU_SYSCLK;
		return src_hz;
	}

	for (n = 2; n <= 64; n++) {
		pre = DIV_ROUND_CLOSEST(src_hz / n, hz);
		if (pre < 1)
			pre = 1;
		if (pre > ESP32S31_SPI_CLKDIV_PRE_MAX)
			pre = ESP32S31_SPI_CLKDIV_PRE_MAX;
		eff = src_hz / (pre * n);
		err = abs((s32)eff - (s32)hz);
		if (err <= best_err) {
			best_err = err;
			best_n = n;
			best_pre = pre;
		}
	}

	reg = FIELD_PREP(ESP32S31_SPI_CLKCNT_N, best_n - 1) |
	      FIELD_PREP(ESP32S31_SPI_CLKDIV_PRE, best_pre - 1) |
	      FIELD_PREP(ESP32S31_SPI_CLKCNT_L, best_n - 1) |
	      FIELD_PREP(ESP32S31_SPI_CLKCNT_H, best_n / 2 - 1);
	*reg_out = reg;
	return src_hz / (best_pre * best_n);
}

static void esp32s31_spi_set_clock(struct esp32s31_spi *esp, u32 hz)
{
	u32 reg;

	if (hz == esp->cur_speed_hz)
		return;

	esp32s31_spi_cal_clock(esp->clk_hz, hz, &reg);
	esp32s31_spi_write(esp, ESP32S31_SPI_CLOCK, reg);
	esp->cur_speed_hz = hz;
}

static void esp32s31_spi_set_mode(struct esp32s31_spi *esp, u8 mode)
{
	u32 user = esp32s31_spi_read(esp, ESP32S31_SPI_USER);
	u32 misc = esp32s31_spi_read(esp, ESP32S31_SPI_MISC);

	if (mode == esp->cur_mode)
		return;

	user &= ~ESP32S31_SPI_CK_OUT_EDGE;
	misc &= ~ESP32S31_SPI_CK_IDLE_EDGE;

	switch (mode & 0x3) {
	case SPI_MODE_0:
		break;
	case SPI_MODE_1:
		user |= ESP32S31_SPI_CK_OUT_EDGE;
		break;
	case SPI_MODE_2:
		misc |= ESP32S31_SPI_CK_IDLE_EDGE;
		user |= ESP32S31_SPI_CK_OUT_EDGE;
		break;
	case SPI_MODE_3:
		misc |= ESP32S31_SPI_CK_IDLE_EDGE;
		break;
	}

	esp32s31_spi_write(esp, ESP32S31_SPI_USER, user);
	esp32s31_spi_write(esp, ESP32S31_SPI_MISC, misc);
	esp->cur_mode = mode;
}

static void esp32s31_spi_apply(struct esp32s31_spi *esp)
{
	u32 cmd;

	esp32s31_spi_write(esp, ESP32S31_SPI_CMD, ESP32S31_SPI_UPDATE);
	do {
		cmd = esp32s31_spi_read(esp, ESP32S31_SPI_CMD);
	} while (cmd & ESP32S31_SPI_UPDATE);
}

static int esp32s31_spi_wait_done(struct esp32s31_spi *esp)
{
	ktime_t deadline = ktime_add_us(ktime_get(), ESP32S31_SPI_DONE_TIMEOUT_US);

	while (!(esp32s31_spi_read(esp, ESP32S31_SPI_DMA_INT_RAW) &
		 ESP32S31_SPI_TRANS_DONE)) {
		if (ktime_after(ktime_get(), deadline))
			return -ETIMEDOUT;
		cpu_relax();
	}
	esp32s31_spi_write(esp, ESP32S31_SPI_DMA_INT_CLR,
			   ESP32S31_SPI_TRANS_DONE);
	return 0;
}

static void esp32s31_spi_fifo_reset(struct esp32s31_spi *esp)
{
	esp32s31_spi_write(esp, ESP32S31_SPI_DMA_CONF,
			   ESP32S31_SPI_BUF_AFIFO_RST | ESP32S31_SPI_RX_AFIFO_RST);
	esp32s31_spi_write(esp, ESP32S31_SPI_DMA_CONF, 0);
}

static void esp32s31_spi_write_fifo(struct esp32s31_spi *esp,
				    const u8 *tx, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < DIV_ROUND_UP(len, 4); i++) {
		u32 word = 0xffffffff;
		unsigned int nbytes = min(len - i * 4, 4u);

		if (tx)
			memcpy(&word, tx + i * 4, nbytes);
		esp32s31_spi_write(esp, ESP32S31_SPI_W0 + i * 4, word);
	}
}

static void esp32s31_spi_read_fifo(struct esp32s31_spi *esp, u8 *rx,
				   unsigned int len)
{
	unsigned int i;

	for (i = 0; i < DIV_ROUND_UP(len, 4); i++) {
		u32 word = esp32s31_spi_read(esp, ESP32S31_SPI_W0 + i * 4);
		unsigned int nbytes = min(len - i * 4, 4u);

		memcpy(rx + i * 4, &word, nbytes);
	}
}

static int esp32s31_spi_chunk(struct esp32s31_spi *esp, const u8 *tx, u8 *rx,
			      unsigned int len)
{
	u32 user;
	int ret;

	esp32s31_spi_fifo_reset(esp);
	esp32s31_spi_write_fifo(esp, tx, len);
	esp32s31_spi_write(esp, ESP32S31_SPI_MS_DLEN,
			   FIELD_PREP(ESP32S31_SPI_MS_DATA_BITLEN, len * 8 - 1));

	/* Always full-duplex: TX FIFO is 0xff when the caller has no tx buf. */
	user = esp32s31_spi_read(esp, ESP32S31_SPI_USER);
	user |= ESP32S31_SPI_DOUTDIN | ESP32S31_SPI_USR_MOSI |
		ESP32S31_SPI_USR_MISO;
	esp32s31_spi_write(esp, ESP32S31_SPI_USER, user);

	esp32s31_spi_write(esp, ESP32S31_SPI_DMA_INT_CLR,
			   ESP32S31_SPI_TRANS_DONE);
	esp32s31_spi_apply(esp);
	esp32s31_spi_write(esp, ESP32S31_SPI_CMD, ESP32S31_SPI_USR);

	ret = esp32s31_spi_wait_done(esp);
	if (ret)
		return ret;

	if (rx)
		esp32s31_spi_read_fifo(esp, rx, len);
	return 0;
}

static int esp32s31_spi_transfer_one(struct spi_controller *host,
				     struct spi_device *spi,
				     struct spi_transfer *xfer)
{
	struct esp32s31_spi *esp = spi_controller_get_devdata(host);
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	unsigned int remaining = xfer->len;
	int ret;

	if (xfer->bits_per_word != 8)
		return -EINVAL;

	esp32s31_spi_set_mode(esp, spi->mode);
	esp32s31_spi_set_clock(esp, xfer->speed_hz ? xfer->speed_hz :
						     spi->max_speed_hz);

	while (remaining) {
		unsigned int chunk = min(remaining, ESP32S31_SPI_FIFO_BYTES);

		ret = esp32s31_spi_chunk(esp, tx, rx, chunk);
		if (ret)
			return ret;
		if (tx)
			tx += chunk;
		if (rx)
			rx += chunk;
		remaining -= chunk;
	}

	return 0;
}

static int esp32s31_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *host;
	struct esp32s31_spi *esp;
	u32 pin;
	int ret;

	host = devm_spi_alloc_host(dev, sizeof(*esp));
	if (!host)
		return -ENOMEM;

	esp = spi_controller_get_devdata(host);
	esp->host = host;
	platform_set_drvdata(pdev, esp);

	esp->regs = devm_platform_ioremap_resource_byname(pdev, "spi");
	if (IS_ERR(esp->regs))
		return PTR_ERR(esp->regs);

	esp->clkrst = devm_platform_ioremap_resource_byname(pdev, "clkrst");
	if (IS_ERR(esp->clkrst))
		return PTR_ERR(esp->clkrst);

	esp->gpio = devm_platform_ioremap_resource_byname(pdev, "gpio");
	if (IS_ERR(esp->gpio))
		return PTR_ERR(esp->gpio);

	esp->iomux = devm_platform_ioremap_resource_byname(pdev, "iomux");
	if (IS_ERR(esp->iomux))
		return PTR_ERR(esp->iomux);

	esp->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(esp->clk))
		return dev_err_probe(dev, PTR_ERR(esp->clk), "clock\n");

	esp->clk_hz = clk_get_rate(esp->clk);
	if (!esp->clk_hz)
		return -EINVAL;

	ret = of_property_read_u32(dev->of_node, "esp,sck-pin", &pin);
	if (ret)
		return dev_err_probe(dev, ret, "esp,sck-pin\n");
	esp->sck_pin = pin;
	ret = of_property_read_u32(dev->of_node, "esp,mosi-pin", &pin);
	if (ret)
		return dev_err_probe(dev, ret, "esp,mosi-pin\n");
	esp->mosi_pin = pin;
	ret = of_property_read_u32(dev->of_node, "esp,miso-pin", &pin);
	if (ret)
		return dev_err_probe(dev, ret, "esp,miso-pin\n");
	esp->miso_pin = pin;

	esp->cur_mode = 0xff;
	esp->cur_speed_hz = 0;

	host->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;
	host->bits_per_word_mask = SPI_BPW_MASK(8);
	host->min_speed_hz = DIV_ROUND_UP(esp->clk_hz,
					  ESP32S31_SPI_CLKDIV_PRE_MAX * 64);
	host->max_speed_hz = esp->clk_hz;
	host->transfer_one = esp32s31_spi_transfer_one;
	host->dev.of_node = dev->of_node;
	host->num_chipselect = 1;
	host->use_gpio_descriptors = true;

	esp32s31_spi_enable_clock(esp);
	esp32s31_spi_hw_init(esp);
	esp32s31_spi_setup_pins(esp);

	ret = devm_spi_register_controller(dev, host);
	if (ret)
		return ret;

	dev_info(dev, "GPSPI3 master, src %u Hz, pins sck=%u mosi=%u miso=%u\n",
		 esp->clk_hz, esp->sck_pin, esp->mosi_pin, esp->miso_pin);
	return 0;
}

static const struct of_device_id esp32s31_spi_of_match[] = {
	{ .compatible = "esp,esp32s31-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_spi_of_match);

static struct platform_driver esp32s31_spi_driver = {
	.driver = {
		.name = "esp32s31-spi",
		.of_match_table = esp32s31_spi_of_match,
	},
	.probe = esp32s31_spi_probe,
};
builtin_platform_driver(esp32s31_spi_driver);

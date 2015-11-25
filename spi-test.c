/*
 *  linux/drivers/spi/spi-test.c
 *
 *  (c) Martin Sperl <kernel@martin.sperl.org>
 *
 *  Simple test driver to test that certain framework optimization
 *  functionality work as expected.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/printk.h>
#include <linux/spi/spi.h>

/* flag to only simulate transfers */
int simulate_only;
module_param(simulate_only, int, 0);
/* dump spi messages */
int dump_messages;
module_param(dump_messages, int, 0);
/* the device is jumpered for loopback - enabling some rx_buf tests */
int loopback;
module_param(loopback, int, 0);

#define SPI_TEST_MAX_TRANSFERS 4
#define SPI_TEST_MAX_SIZE (32 * PAGE_SIZE)
#define SPI_TEST_MAX_ITERATE 12

/* the "dummy" start addresses used in spi_test
 * these addresses get translated at a later stage
 */
#define RX_START	BIT(30)
#define TX_START	BIT(31)
#define RX(off)		((void *)(RX_START + off))
#define TX(off)		((void *)(TX_START + off))

/* we allocate one page more, to allow for offsets */
#define SPI_TEST_MAX_SIZE_PLUS (SPI_TEST_MAX_SIZE + PAGE_SIZE)

static int spi_test_execute_msg(struct spi_device *spi,
				struct spi_message *msg);

/* describes a specific (set of) tests to get executed */
struct spi_test {
	char description[64];	/* a description of the test */
	/* iterate over all the non-zero values */
	int iterate_len[SPI_TEST_MAX_ITERATE]; /* set the transfer length  */
	int iterate_tx_align; /* test dma_alignment - use as value if 0 */
	int iterate_rx_align; /* test dma_alignment - use as value if 0 */
	int (*test)(struct spi_test *test,
		    struct spi_device *spi,
		    struct spi_message *msg,
		    void *tx, void *rx); /* custom test code */
	int expected_return;    /* typically 0, but to test error cases */
	unsigned int transfer_count; /* typically 0 - calculated */
	struct spi_transfer transfers[SPI_TEST_MAX_TRANSFERS];
	u32 fill;		/* fill tx with these 32bit pattern */
	u32 fill_option;	/* fill options */
	/* fill definitions */
#define FILL_MEMSET_8	0	/* just memset with 8 bit */
#define FILL_MEMSET_16	1	/* just memset with 16 bit */
#define FILL_MEMSET_24	2	/* just memset with 24 bit */
#define FILL_MEMSET_32	3	/* just memset with 32 bit */
#define FILL_COUNT_8	4	/* fill with a 8 byte counter */
#define FILL_COUNT_16	5	/* fill with a 16 bit counter */
#define FILL_COUNT_24	6	/* fill with a 24 bit counter */
#define FILL_COUNT_32	7	/* fill with a 32 bit counter */
#define FILL_TRANSFER_BYTE_8  8	/* fill with the transfer byte - 8 bit */
#define FILL_TRANSFER_BYTE_16 9	/* fill with the transfer byte - 16 bit */
#define FILL_TRANSFER_BYTE_24 10 /* fill with the transfer byte - 24 bit */
#define FILL_TRANSFER_BYTE_32 11 /* fill with the transfer byte - 32 bit */
#define FILL_TRANSFER_NUM     16 /* fill with the transfer number */
};

#define ITERATE_LEN {16, 32, 64, 128, 256, 1024, PAGE_SIZE, \
		     SPI_TEST_MAX_SIZE, }
#define ITERATE_ALIGN sizeof(int)

static struct spi_test spi_tests[] = {
	{
		.description	= "tx/rx-transfer - start of page",
		.fill_option	= FILL_COUNT_8,
		.iterate_len    = ITERATE_LEN,
		.iterate_tx_align = ITERATE_ALIGN,
		.iterate_rx_align = ITERATE_ALIGN,
		.transfers		= {
			{
				.len = 1,
				.tx_buf = TX(0),
				.rx_buf = RX(0),
			},
		},
	},
	{
		.description	= "tx/rx-transfer - crossing PAGE_SIZE",
		.fill_option	= FILL_COUNT_8,
		.iterate_len    = ITERATE_LEN,
		.iterate_tx_align = ITERATE_ALIGN,
		.iterate_rx_align = ITERATE_ALIGN,
		.transfers		= {
			{
				.len = 1,
				.tx_buf = TX(PAGE_SIZE - 4),
				.rx_buf = RX(PAGE_SIZE - 4),
			},
		},
	},
	{
		.description	= "tx-transfer - only",
		.fill_option	= FILL_COUNT_8,
		.iterate_len    = ITERATE_LEN,
		.iterate_tx_align = ITERATE_ALIGN,
		.transfers		= {
			{
				.len = 1,
				.tx_buf = TX(0),
			},
		},
	},
	{
		.description	= "rx-transfer - only",
		.fill_option	= FILL_COUNT_8,
		.iterate_len    = ITERATE_LEN,
		.iterate_rx_align = ITERATE_ALIGN,
		.transfers		= {
			{
				.len = 1,
				.rx_buf = RX(0),
			},
		},
	},
	{ /* end of tests sequence */ }
};

static void spi_test_dump_message(struct spi_device *spi,
				  struct spi_message *msg,
				  bool dump_data)
{
	struct spi_transfer *xfer;

	dev_info(&spi->dev, "  spi_msg@%pK\n", msg);
	if (msg->status)
		dev_info(&spi->dev, "    status:        %i\n",
			 msg->status);
	dev_info(&spi->dev, "    frame_length:  %i\n",
		 msg->frame_length);
	dev_info(&spi->dev, "    actual_length: %i\n",
		 msg->actual_length);

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		dev_info(&spi->dev, "    spi_transfer@%pK\n", xfer);
		dev_info(&spi->dev, "      len:    %i\n", xfer->len);
		dev_info(&spi->dev, "      tx_buf: %pK\n", xfer->tx_buf);
		if (dump_data && xfer->tx_buf)
			print_hex_dump(KERN_INFO, "          TX: ",
				       DUMP_PREFIX_OFFSET, 16, 1,
				       xfer->tx_buf, xfer->len, 0);

		dev_info(&spi->dev, "      rx_buf: %pK\n", xfer->rx_buf);
		if (dump_data && xfer->rx_buf)
			print_hex_dump(KERN_INFO, "          RX: ",
				       DUMP_PREFIX_OFFSET, 16, 1,
				       xfer->tx_buf, xfer->len, 0);
	}
}

static int spi_test_check_loopback_result(struct spi_device *spi,
					  struct spi_message *msg)
{
	struct spi_transfer *xfer;
	u8 rxb, txb;
	size_t i;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		/* if there is no rx, then no check is needed */
		if (!xfer->rx_buf)
			continue;
		/* so depending on tx_buf we need to handle things */
		if (xfer->tx_buf) {
			for (i = 1; i < xfer->len; i++) {
				txb = ((u8 *)xfer->tx_buf)[i];
				rxb = ((u8 *)xfer->rx_buf)[i];
				if (txb != rxb)
					goto mismatch_error;
			}
		} else {
			/* first byte received */
			txb = ((u8 *)xfer->rx_buf)[0];
			/* first byte may be 0 or xff */
			if (!((txb == 0) || (txb == 0xff))) {
				dev_err(&spi->dev,
					"loopback strangeness - we expect 0x00 or 0xff, but not 0x%02x\n",
					txb);
				return -EINVAL;
			}
			/* check that all bytes are identical */
			for (i = 1; i < xfer->len; i++) {
				rxb = ((u8 *)xfer->rx_buf)[i];
				if (rxb != txb)
					goto mismatch_error;
			}
		}
	}

	return 0;

mismatch_error:
	dev_err(&spi->dev,
		"loopback strangeness - transfer missmatch on byte %i - expected 0x%02x, but got 0x%02x\n",
		i, txb, rxb);

	return -EINVAL;
}

static int spi_test_execute_msg(struct spi_device *spi,
				struct spi_message *msg)
{
	int ret = 0;

	if (!simulate_only) {
		/* run spi message */
		ret = spi_sync(spi, msg);
		if (ret) {
			dev_err(&spi->dev,
				"Failed to execute spi_message: %i\n",
				ret);
			goto exit;
		}

		/* do some extra error checks */
		if (msg->frame_length != msg->actual_length) {
			dev_err(&spi->dev,
				"actual length differs from expected\n");
			ret = -EIO;
			goto exit;
		}

		/* run rx-tests when in loopback mode */
		if (loopback)
			ret = spi_test_check_loopback_result(spi, msg);
	}

	/* if requested or on error dump message (including data) */
exit:
	if (dump_messages || ret)
		spi_test_dump_message(spi, msg,
				      (dump_messages == 2) || (ret));

	return ret;
}

static int spi_test_translate(struct spi_device *spi,
			      void **ptr, size_t len,
			      void *tx, void *rx)
{
	size_t off;

	/* on NULL pointer, there is nothing to do */
	if (!(*ptr))
		return 0;

	/* RX range */
	if ((*ptr >= RX(0)) && (*ptr + len <= RX(SPI_TEST_MAX_SIZE_PLUS))) {
		off = *ptr - RX(0);
		*ptr = rx + off;

		return 0;
	}

	/* TX range */
	if ((*ptr >= TX(0)) && (*ptr + len <= TX(SPI_TEST_MAX_SIZE_PLUS))) {
		off = *ptr - TX(0);
		*ptr = tx + off;

		return 0;
	}

	dev_err(&spi->dev,
		"PointerRange [%pK:%pK[ not in range [%pK:%pK[ or [%pK:%pK[\n",
		*ptr, *ptr + len,
		RX(0), RX(SPI_TEST_MAX_SIZE),
		TX(0), TX(SPI_TEST_MAX_SIZE));

	return -EINVAL;
}

static int spi_test_fill_tx(struct spi_test *test, struct spi_device *spi)
{
	struct spi_transfer *xfers = test->transfers;
	u8 *tx_buf;
	size_t len, count = 0;
	int i, j;

#ifdef __BIG_ENDIAN
#define GET_VALUE_BYTE(value, index, bytes) \
	(value >> (8 * (bytes - 1 - count % bytes)))
#else
#define GET_VALUE_BYTE(value, index, bytes) \
	(value >> (8 * (count % bytes)))
#endif

	/* fill all transfers with the pattern requested */
	for (i = 0; i < test->transfer_count; i++) {
		/* if tx_buf is NULL then skip */
		tx_buf = (u8 *)xfers[i].tx_buf;
		if (!tx_buf)
			continue;
		len = xfers[i].len;
		/* modify all the transfers */
		for (j = 0; j < len; j++, tx_buf++, count++) {
			switch (test->fill_option) {
			case FILL_MEMSET_8:
				*tx_buf = test->fill;
				break;
			case FILL_MEMSET_16:
				*tx_buf = GET_VALUE_BYTE(test->fill, count, 2);
				break;
			case FILL_MEMSET_24:
				*tx_buf = GET_VALUE_BYTE(test->fill, count, 3);
				break;
			case FILL_MEMSET_32:
				*tx_buf = GET_VALUE_BYTE(test->fill, count, 4);
				break;
			case FILL_COUNT_8:
				*tx_buf = count;
				break;
			case FILL_COUNT_16:
				*tx_buf = GET_VALUE_BYTE(count, count, 2);
				break;
			case FILL_COUNT_24:
				*tx_buf = GET_VALUE_BYTE(count, count, 3);
				break;
			case FILL_COUNT_32:
				*tx_buf = GET_VALUE_BYTE(count, count, 4);
				break;
			case FILL_TRANSFER_BYTE_8:
				*tx_buf = j;
				break;
			case FILL_TRANSFER_BYTE_16:
				*tx_buf = GET_VALUE_BYTE(j, j, 2);
				break;
			case FILL_TRANSFER_BYTE_24:
				*tx_buf = GET_VALUE_BYTE(j, j, 3);
				break;
			case FILL_TRANSFER_BYTE_32:
				*tx_buf = GET_VALUE_BYTE(j, j, 4);
				break;
			case FILL_TRANSFER_NUM:
				*tx_buf = i;
				break;
			default:
				dev_err(&spi->dev,
					"unsupported fill_option: %i\n",
					test->fill_option);
				return -EINVAL;
			}
		}
	}

	return 0;
}

int _spi_test_run(struct spi_device *spi,
		  struct spi_test *test,
		  void *tx, void *rx)
{
	struct spi_message msg; /* ideally we could use test.message */
	struct spi_transfer *x;
	int i, ret;

	/* initialize message */
	spi_message_init(&msg);

	/* add the individual transfers */
	for (i = 0; i < test->transfer_count; i++) {
		x = &test->transfers[i];
		/* copy values */
		memcpy(x, &test->transfers[i], sizeof(*x));

		/* patch the values of rx_buf/tx_buf */
		ret = spi_test_translate(spi, (void **)&x->tx_buf, x->len,
					 (void *)tx, rx);
		if (ret)
			return ret;

		ret = spi_test_translate(spi, &x->rx_buf, x->len,
					 (void *)tx, rx);
		if (ret)
			return ret;

		spi_message_add_tail(x, &msg);
	}

	/* fill in the transfer data */
	ret = spi_test_fill_tx(test, spi);
		if (ret)
			return ret;

	/* and execute */
	if (test->test)
		ret = test->test(test, spi, &msg, tx, rx);
	else
		ret = spi_test_execute_msg(spi, &msg);

	/* handle result */
	if (ret == test->expected_return)
		return 0;

	dev_err(&spi->dev,
		"test failed - test returned %i, but we expect %i\n",
		ret, test->expected_return);

	if (ret)
		return ret;

	/* if it is 0, as we expected something else,
	 * then return something special
	 */
	return -EFAULT;
}

static int spi_test_run_iter(struct spi_device *spi,
			     const struct spi_test *testtemplate,
			     void *tx, void *rx,
			     size_t len,
			     size_t tx_off,
			     size_t rx_off
	)
{
	struct spi_test test;
	int i, tx_count, rx_count;

	/* copy the test template to test */
	memcpy(&test, testtemplate, sizeof(test));

	/* set up test->transfers to the correct count */
	if (!test.transfer_count) {
		for (i = 0;
		    (i < SPI_TEST_MAX_TRANSFERS) && test.transfers[i].len;
		    i++) {
			test.transfer_count++;
		}
	}

	/* count number of transfers with tx/rx_buf != NULL */
	for (i = 0; i < test.transfer_count; i++) {
		if (test.transfers[i].tx_buf)
			tx_count++;
		if (test.transfers[i].rx_buf)
			rx_count++;
	}

	/* in some iteration cases warn and exit early,
	 * as there is nothing to do, that has not been tested already...
	 */
	if (tx_off && (!tx_count)) {
		dev_warn_once(&spi->dev,
			      "%s: iterate_tx_off configured with tx_buf==NULL - ignoring\n",
			      test.description);
		return 0;
	}
	if (rx_off && (!rx_count)) {
		dev_warn_once(&spi->dev,
			      "%s: iterate_rx_off configured with rx_buf==NULL - ignoring\n",
			      test.description);
		return 0;
	}

	/* write out info */
	if (!(len || tx_off || rx_off)) {
		dev_info(&spi->dev, "Running test %s\n", test.description);
	} else {
		dev_info(&spi->dev,
			 "  with iteration values: len = %i, tx_off = %i, rx_off = %i\n",
			 len, tx_off, rx_off);

		/* update in the values from iteration values */
		for (i = 0; i < test.transfer_count; i++) {
			if (len)
				test.transfers[i].len = len;
			if (test.transfers[i].tx_buf)
				test.transfers[i].tx_buf += tx_off;
			if (test.transfers[i].tx_buf)
				test.transfers[i].rx_buf += rx_off;
		}
	}

	/* and execute */
	return _spi_test_run(spi, &test, tx, rx);
}

static int spi_test_run(struct spi_device *spi,
			void *tx, void *rx,
			const struct spi_test *test)
{
	int idx_len;
	size_t len;
	size_t tx_align, rx_align;
	int ret;

	/* test for transfer limits */
	if (test->transfer_count >= SPI_TEST_MAX_TRANSFERS) {
		dev_err(&spi->dev,
			"%s: Exceeded max number of transfers with %i\n",
			test->description, test->transfer_count);
		return -E2BIG;
	}

	/* iterate over all the iterable values using macros
	 * (to make it a bit more readable...
	 */
#define FOR_EACH_ITERATE(var, defaultvalue)				\
	for (idx_##var = -1, var = defaultvalue;			\
	     ((idx_##var < 0) ||					\
		     (							\
			     (idx_##var < SPI_TEST_MAX_ITERATE) &&	\
			     (var = test->iterate_##var[idx_##var])	\
		     )							\
	     );								\
	     idx_##var++)
#define FOR_EACH_ALIGNMENT(var)						\
	for (var = 0;							\
	    var < (test->iterate_##var ?				\
			(spi->master->dma_alignment ?			\
			 spi->master->dma_alignment :			\
			 test->iterate_##var) :				\
			1);						\
	    var++)

	FOR_EACH_ITERATE(len, 0) {
		FOR_EACH_ALIGNMENT(tx_align) {
			FOR_EACH_ALIGNMENT(rx_align) {
				/* and run the iteration */
				ret = spi_test_run_iter(spi, test,
							tx, rx,
							len,
							tx_align,
							rx_align);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static int spi_test_probe(struct spi_device *spi)
{
	char *rx = NULL, *tx = NULL;
	int ret = 0;
	struct spi_test *test;

	dev_info(&spi->dev, "Executing spi-tests\n");

	/* allocate rx/tx buffers of 128kB size without devm
	 * in the hope that is on a page boundary
	 */
	rx = kzalloc(SPI_TEST_MAX_SIZE_PLUS, GFP_KERNEL);
	if (!rx) {
		ret = -ENOMEM;
		goto out;
	}

	tx = kzalloc(SPI_TEST_MAX_SIZE_PLUS, GFP_KERNEL);
	if (!tx) {
		ret = -ENOMEM;
		goto out;
	}

	/* now run the individual tests in the table */
	for (test = spi_tests; test->description[0]; test = &test[1]) {
		ret = spi_test_run(spi, tx, rx, test);
		if (ret)
			goto out;
	}

out:
	dev_info(&spi->dev, "Finished spi-tests with return: %i\n", ret);
	kfree(rx);
	kfree(tx);

	return ret;
}

/* non const match table to permit to change via a module parameter */
static struct of_device_id spi_test_of_match[] = {
	{ .compatible	= "spi,loopback-test", },
	{ }
};

/* allow to override the compatible string via a module_parameter */
module_param_string(compatible, spi_test_of_match[0].compatible,
		    sizeof(spi_test_of_match[0].compatible), 0000);

MODULE_DEVICE_TABLE(of, spi_test_of_match);

static struct spi_driver spi_test_driver = {
	.driver = {
		.name = "spi-test",
		.owner = THIS_MODULE,
		.of_match_table = spi_test_of_match,
	},
	.probe = spi_test_probe,
};

module_spi_driver(spi_test_driver);

MODULE_AUTHOR("Martin Sperl <kernel@martin.sperl.org>");
MODULE_DESCRIPTION("test spi_driver to check core functionality");
MODULE_LICENSE("GPL");

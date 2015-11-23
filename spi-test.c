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
#include <linux/spi/spi.h>

int simulate_only = 0;
module_param(simulate_only, int, 0);
int dump_messages = 0;
module_param(dump_messages, int, 0);

#define SPI_TEST_MAX_TRANSFERS 4
#define SPI_TEST_MAX_SIZE (32 * PAGE_SIZE)
#define SPI_TEST_MAX_ITERATE 12

#define RX_START	(1<<30)
#define RX(off)		((void *)(RX_START + off))
#define TX_START	(2<<30)
#define TX(off)		((void *)(TX_START + off))

static int spi_test_execute_msg(struct spi_device *spi,
				struct spi_message *msg);

/* describes a specific (set of) tests to get executed */
struct spi_test {
	char description[64];	/* a description of the test */
	/* iterate over all the non-zero values */
	int iterate_len[SPI_TEST_MAX_ITERATE]; /* set the transfer length  */
	int iterate_tx_off[SPI_TEST_MAX_ITERATE]; /* shift tx_buff by this */
	int iterate_rx_off[SPI_TEST_MAX_ITERATE]; /* shift rx_buf by this */
	int (*test)(struct spi_test *test,
		    struct spi_device *spi,
		    struct spi_message *msg,
		    void *tx, void *rx); /* custom test code */
	int expected_return;    /* typically 0, but to test error cases */
	unsigned int transfers; /* # of transfers < SPI_TEST_MAX_TRANSFERS */
	struct spi_transfer xfers[SPI_TEST_MAX_TRANSFERS]; /* the transfers */
	u32 fill;		/* fill tx with these 32bit pattern */
	u32 fill_option;	/* fill options */
	/* magic fill pattern */
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

static struct spi_test spi_tests[] = {
	{
		.description	= "simple tx-transfer",
		.iterate_len    = { 16, 32, 64, 128, 256, 1024, PAGE_SIZE, 32768, 65536, SPI_TEST_MAX_SIZE},
		.iterate_tx_off = { 1, 2, 3, 4, 5, 6, 7},
		.transfers	= 1,
		.xfers		= {
			{
				.len = 1,
				.tx_buf = TX(0),
			},
		},
	},
	{
		.description	= "simple rx-transfer",
		.iterate_len    = { 16, 32, 64, 128, 256, 1024, PAGE_SIZE, 32768, 65536, SPI_TEST_MAX_SIZE},
		.iterate_rx_off = { 1, 2, 3, 4, 5, 6, 7},
		.transfers	= 1,
		.xfers		= {
			{
				.len = 1,
				.rx_buf = RX(0),
			},
		},
	},
	{
		.description	= "simple tx/rx-transfer",
		.iterate_len    = { 16, 32, 64, 128, 256, 1024, PAGE_SIZE, 32768, 65536, SPI_TEST_MAX_SIZE},
		.iterate_tx_off = { 1, 2, 3, 4, 5, 6, 7},
		.iterate_rx_off = { 1, 2, 3, 4, 5, 6, 7},
		.transfers	= 1,
		.xfers		= {
			{
				.len = 1,
				.tx_buf = TX(0),
				.rx_buf = RX(0),
			},
		},
	},

	{ }
};

static void spi_test_dump_message(struct spi_device *spi,
				  struct spi_message *msg)
{
	struct spi_transfer *xfer;

	dev_info(&spi->dev,"  spi_msg@%pK\n", msg);

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		dev_info(&spi->dev, "    spi_transfer@%pK\n", xfer);
		dev_info(&spi->dev, "      len:    %i\n", xfer->len);
		dev_info(&spi->dev, "      tx_buf: %pK\n", xfer->tx_buf);
		dev_info(&spi->dev, "      rx_buf: %pK\n", xfer->rx_buf);
	}
}

static int spi_test_execute_msg(struct spi_device *spi,
				struct spi_message *msg)
{
	int ret;
	if (dump_messages)
		spi_test_dump_message(spi, msg);

	if (simulate_only)
		return 0;

	return spi_sync(spi, msg);
}

static int spi_test_translate(struct spi_device *spi,
			      void **ptr, size_t len,
			      void *tx, void *rx)
{
	size_t off;

	/* NULL pointer */
	if (! *ptr)
		return 0;

	/* RX range */
	if ((*ptr >= RX(0)) && ( *ptr + len < RX(SPI_TEST_MAX_SIZE))) {
		off = *ptr - RX(0);
		*ptr = rx + off;

		return 0;
	}

	/* TX range */
	if ((*ptr >= TX(0)) && ( *ptr + len < TX(SPI_TEST_MAX_SIZE))) {
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

static int spi_test_fill_tx(struct spi_test *test,
			    struct spi_device *spi,
			    struct spi_message *msg
			    )
{
	struct spi_transfer *xfers = test->xfers;
	char *cbuf;
	size_t len, count = 0;
	int i, j;

#ifdef __BIG_ENDIAN
#define GET_VALUE_BYTE(value, index, bytes) \
	value >> (8 * (bytes -1 - count % bytes))
#else
#define GET_VALUE_BYTE(value, index, bytes) \
	value >> (8 * (count % bytes))
#endif

	for(i = 0; i < test->transfers; i++) {
		cbuf = (char *)xfers[i].tx_buf;
		len = xfers[i].len;
		for (j = 0; j < len; j++, cbuf++, count++) {
			switch (test->fill_option) {
			case FILL_MEMSET_8:
				*cbuf = test->fill;
				break;
			case FILL_MEMSET_16:
				*cbuf = GET_VALUE_BYTE(test->fill, count, 2);
				break;
			case FILL_MEMSET_24:
				*cbuf = GET_VALUE_BYTE(test->fill, count, 3);
				break;
			case FILL_MEMSET_32:
				*cbuf = GET_VALUE_BYTE(test->fill, count, 4);
				break;
			case FILL_COUNT_8:
				*cbuf = count;
				count++;
				break;
			case FILL_COUNT_16:
				*cbuf = GET_VALUE_BYTE(count, count, 2);
				count++;
				break;
			case FILL_COUNT_24:
				*cbuf = GET_VALUE_BYTE(count, count, 3);
				count++;
				break;
			case FILL_COUNT_32:
				*cbuf = GET_VALUE_BYTE(count, count, 4);
				count++;
				break;
			case FILL_TRANSFER_BYTE_8:
				*cbuf = j;
				break;
			case FILL_TRANSFER_BYTE_16:
				*cbuf = GET_VALUE_BYTE(j, j, 2);
				break;
			case FILL_TRANSFER_BYTE_24:
				*cbuf = GET_VALUE_BYTE(j, j, 3);
				break;
			case FILL_TRANSFER_BYTE_32:
				*cbuf = GET_VALUE_BYTE(j, j, 4);
				break;
			case FILL_TRANSFER_NUM:
				*cbuf = i;
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

	/* test for transfers */
	if (test->transfers >= SPI_TEST_MAX_TRANSFERS) {
		dev_err(&spi->dev,
			"Exceeded max number of allowed transfers of %i with %i\n",
			SPI_TEST_MAX_TRANSFERS,
			test->transfers);
		return -E2BIG;
	}

	/* initialize message */
	spi_message_init(&msg);

	/* add the individual transfers */
	for(i = 0; i < test->transfers; i++) {
		x = &test->xfers[i];
		/* copy values */
		memcpy(x, &test->xfers[i], sizeof(*x));

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
	ret = spi_test_fill_tx(test, spi, &msg);
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
	int i;

	/* copy the test template to test */
	memcpy(&test, testtemplate, sizeof(test));

	/* write out info */
	if (!(len || tx_off || rx_off)) {
		dev_info(&spi->dev,
			"Running test %s\n",
			test.description);
	} else {
		dev_info(&spi->dev,
			"  with iteration values: len = %i, tx_off = %i, rx_off = %i\n",
			len, tx_off, rx_off);
	}


	/* fill in the values from iterations - if set...*/
	for(i = 0; i < test.transfers; i++) {
		if (len)
			test.xfers[i].len = len;
		if (tx_off && test.xfers[i].tx_buf)
			test.xfers[i].tx_buf += tx_off;
		if (rx_off && test.xfers[i].rx_buf)
			test.xfers[i].rx_buf += rx_off;
	}

	/* and execute */
	return _spi_test_run(spi, &test, tx, rx);
}

static int spi_test_run(struct spi_device *spi,
			void *tx, void *rx,
			const struct spi_test *test)
{
	int idx_len, idx_tx_off, idx_rx_off;
	size_t len, tx_off, rx_off;
	int ret;

	if (test->iterate_len[0] ||
	    test->iterate_rx_off[0] ||
	    test->iterate_rx_off[0]) {
		dev_info(&spi->dev,"Running test %s with iterations\n",
			 test->description);
	}

	/* iterate over all the iterable values using a macro */
#define FOR_EACH_ITERATE(var, defaultvalue)				\
	for (idx_##var = -1, var = defaultvalue;			\
	     ((idx_##var < 0) ||					\
		     (							\
			     (idx_##var < SPI_TEST_MAX_ITERATE) &&	\
			     (var = test->iterate_##var[idx_##var])	\
		     )							\
       	     );								\
	     idx_##var ++)

	FOR_EACH_ITERATE(len, 0) {
		FOR_EACH_ITERATE(tx_off, 0) {
			FOR_EACH_ITERATE(rx_off, 0) {
				/* and run the iteration */
				ret = spi_test_run_iter(spi, test,
							tx, rx,
							len,
							tx_off,
							rx_off);
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
	 * in the hope that is on a page boundry
	 */
	rx = kzalloc(SPI_TEST_MAX_SIZE, GFP_KERNEL);
	if (!rx) {
		ret = -ENOMEM;
		goto out;
	}

	tx = kzalloc(SPI_TEST_MAX_SIZE, GFP_KERNEL);
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
	dev_info(&spi->dev, "Finished spi-tests with exit %i\n", ret);
	kfree(rx);
	kfree(tx);

	return ret;
}

/* define the match table */
static struct of_device_id spi_test_of_match[] = {
	{
		.compatible	= "linux,spi-test",
	},
	{ }
};
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

module_param_string(compatible, spi_test_of_match[0].compatible,
		    sizeof(spi_test_of_match[0].compatible), 0000);


MODULE_AUTHOR("Martin Sperl <kernel@martin.sperl.org>");
MODULE_DESCRIPTION("test spi_driver to check core functionality");
MODULE_LICENSE("GPL");

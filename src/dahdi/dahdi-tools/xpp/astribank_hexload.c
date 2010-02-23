/*
 * Written by Oron Peled <oron@actcom.co.il>
 * Copyright (C) 2008, Xorcom
 *
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <arpa/inet.h>
#include "debug.h"
#include "hexfile.h"
#include "mpp_funcs.h"
#include "pic_loader.h"
#include "astribank_usb.h"

#define	DBG_MASK	0x80
#define	MAX_HEX_LINES	10000

static char	*progname;

static void usage()
{
	fprintf(stderr, "Usage: %s [options...] -D {/proc/bus/usb|/dev/bus/usb}/<bus>/<dev> hexfile...\n", progname);
	fprintf(stderr, "\tOptions: {-F|-p}\n");
	fprintf(stderr, "\t\t[-E]               # Burn to EEPROM\n");
	fprintf(stderr, "\t\t[-F]               # Load FPGA firmware\n");
	fprintf(stderr, "\t\t[-p]               # Load PIC firmware\n");
	fprintf(stderr, "\t\t[-v]               # Increase verbosity\n");
	fprintf(stderr, "\t\t[-d mask]          # Debug mask (0xFF for everything)\n");
	exit(1);
}

int handle_hexline(struct astribank_device *astribank, struct hexline *hexline)
{
	uint16_t	len;
	uint16_t	offset_dummy;
	uint8_t		*data;
	int		ret;

	assert(hexline);
	assert(astribank);
	if(hexline->d.content.header.tt != TT_DATA) {
		DBG("Non data record type = %d\n", hexline->d.content.header.tt);
		return 0;
	}
	len = hexline->d.content.header.ll;
	offset_dummy = hexline->d.content.header.offset;
	data = hexline->d.content.tt_data.data;
	if((ret = mpp_send_seg(astribank, data, offset_dummy, len)) < 0) {
		ERR("Failed hexfile send line: %d\n", ret);
		return -EINVAL;
	}
	return 0;
}

static int load_hexfile(struct astribank_device *astribank, const char *hexfile, enum dev_dest dest)
{
	struct hexdata		*hexdata = NULL;
	int			finished = 0;
	int			ret;
	int			i;
	char			star[] = "+\\+|+/+-";

	if((hexdata  = parse_hexfile(hexfile, MAX_HEX_LINES)) == NULL) {
		perror(hexfile);
		return -errno;
	}
	INFO("Loading hexfile to %s: %s (version %s)\n",
		dev_dest2str(dest),
		hexdata->fname, hexdata->version_info);
#if 0
	FILE		*fp;
	if((fp = fopen("fpga_dump_new.txt", "w")) == NULL) {
		perror("dump");
		exit(1);
	}
#endif
	if((ret = mpp_send_start(astribank, dest, hexdata->version_info)) < 0) {
		ERR("Failed hexfile send start: %d\n", ret);
		return ret;
	}
	for(i = 0; i < hexdata->maxlines; i++) {
		struct hexline	*hexline = hexdata->lines[i];

		if(!hexline)
			break;
		if(verbose > LOG_INFO) {
			printf("Sending: %4d%%    %c\r", (100 * i) / hexdata->last_line, star[i % sizeof(star)]);
			fflush(stdout);
		}
		if(finished) {
			ERR("Extra data after End Of Data Record (line %d)\n", i);
			return 0;
		}
		if(hexline->d.content.header.tt == TT_EOF) {
			DBG("End of data\n");
			finished = 1;
			continue;
		}
		if((ret = handle_hexline(astribank, hexline)) < 0) {
			ERR("Failed hexfile sending in lineno %d (ret=%d)\n", i, ret);;
			return ret;
		}
	}
	if(verbose > LOG_INFO) {
		putchar('\n');
		fflush(stdout);
	}
	if((ret = mpp_send_end(astribank)) < 0) {
		ERR("Failed hexfile send end: %d\n", ret);
		return ret;
	}
#if 0
	fclose(fp);
#endif
	free_hexdata(hexdata);
	DBG("hexfile loaded successfully\n");
	return 0;
}

int main(int argc, char *argv[])
{
	char			*devpath = NULL;
	struct astribank_device *astribank;
	int			opt_pic = 0;
	int			opt_dest = 0;
	enum dev_dest		dest = DEST_NONE;
	const char		options[] = "vd:D:EFp";
	int			iface_num;
	int			ret;

	progname = argv[0];
	while (1) {
		int	c;

		c = getopt (argc, argv, options);
		if (c == -1)
			break;

		switch (c) {
			case 'D':
				devpath = optarg;
				break;
			case 'E':
				if(dest != DEST_NONE) {
					ERR("The -F and -E options are mutually exclusive.\n");
					usage();
				}
				opt_dest = 1;
				dest = DEST_EEPROM;
				break;
			case 'F':
				if(dest != DEST_NONE) {
					ERR("The -F and -E options are mutually exclusive.\n");
					usage();
				}
				opt_dest = 1;
				dest = DEST_FPGA;
				break;
			case 'p':
				opt_pic = 1;
				break;
			case 'v':
				verbose++;
				break;
			case 'd':
				debug_mask = strtoul(optarg, NULL, 0);
				break;
			case 'h':
			default:
				ERR("Unknown option '%c'\n", c);
				usage();
		}
	}
	if((opt_dest ^ opt_pic) == 0) {
		ERR("The -F, -E and -p options are mutually exclusive.\n");
		usage();
	}
	iface_num = (opt_dest) ? 1 : 0;
	if(!opt_pic) {
		if(optind != argc - 1) {
			ERR("Got %d hexfile names (Need exactly one hexfile)\n",
				argc - 1 - optind);
			usage();
		}
	}
	if(!devpath) {
		ERR("Missing device path.\n");
		usage();
	}
	if((astribank = astribank_open(devpath, iface_num)) == NULL) {
		ERR("Opening astribank failed\n");
		return 1;
	}
	show_astribank_info(astribank);
	if(opt_dest) {
		if(load_hexfile(astribank, argv[optind], dest) < 0) {
			ERR("Loading firmware to %s failed\n", dev_dest2str(dest));
			return 1;
		}
	} else if(opt_pic) {
		if((ret = load_pic(astribank, argc - optind, argv + optind)) < 0) {
			ERR("Loading PIC's failed\n");
			return 1;
		}
	}
	astribank_close(astribank, 0);
	return 0;
}

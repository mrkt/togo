/*
 * command.c
 *
 * Created on: 2015-6-16
 * Author: zhuli
 */
#include "togo.h"
#include "togo_load.h"

static int togo_command_split(u_char *command, TOGO_COMMAND_TAG *command_tag);
static void togo_command_close_read_big_data(TOGO_THREAD_ITEM * socket_item);

enum TOGO_READ_NETWORK togo_command_read_network(struct bufferevent *bev,
		TOGO_THREAD_ITEM * socket_item)
{
	enum TOGO_READ_NETWORK ret = READ_NO_DATA_RECEIVED;
	int sfd = socket_item->sfd;
	if (socket_item->rbuf != socket_item->rcurr) {
		if (socket_item->rbytes != 0) {
			togo_memmove(socket_item->rbuf, socket_item->rcurr,
					socket_item->rbytes);
		}
		socket_item->rcurr = socket_item->rbuf;
	}

	int realloc_num = 0;
	while (1) {

		/* if the space of rbuf is used up, realloc the socket_item */
		if (socket_item->rbytes >= socket_item->rsize) {

			if (realloc_num == 4 || socket_item->rsize >= TOGO_S_RBUF_MAX_SIZE) {
				return ret;
			}
			++realloc_num;

			u_char *new_rbuf = togo_pool_realloc(socket_item->worker_pool,
					(void *) socket_item->rbuf, socket_item->rsize,
					socket_item->rsize * 2);
			if (!new_rbuf) {
				togo_log(INFO, "Realloc socket_item error.");
				return READ_ERROR;
			}

			socket_item->rcurr = socket_item->rbuf = new_rbuf;
			socket_item->rsize *= 2;
		}

		/* Try to read network data by Bufferevent */
		int free_space = socket_item->rsize - socket_item->rbytes;
		int len;
		len = bufferevent_read(bev, socket_item->rbuf + socket_item->rbytes,
				free_space);

		if (len > 0) {
			socket_item->rbytes += len;
			ret = READ_DATA_RECEIVED;

			if (free_space == len) {
				continue;
			} else {
				break;
			}

		} else {
			return READ_ERROR;
		}
	}
	return ret;
}

BOOL togo_command_parse_command(TOGO_THREAD_ITEM * socket_item,
		SEND_CALLBACK togo_wt_send_cb)
{
	/* There is nothing to parse.*/
	if (socket_item->rbytes == 0) {
		return FALSE;
	}

	u_char *tag, *new_curr;
	tag = togo_memchr(socket_item->rcurr, TOGO_S_MSG_TAG, socket_item->rbytes);
	if (!tag) {
		return FALSE;
	}

	new_curr = tag + 1; /* next rbuf_curr*/
	if ((tag - socket_item->rcurr) > 1 && *(tag - 1) == '\r') {
		tag--;
	}
	*tag = '\0';

	/* Split the command */
	TOGO_COMMAND_TAG command_tag[TOGO_COMMAND_TAG_MAX];
	int ntag = togo_command_split(socket_item->rcurr, command_tag);

	/* Command is too large! */
	if (ntag == -1) {
		togo_command_build_send(socket_item, TOGO_SBUF_COMMAND_TOO_BIG,
				togo_strlen(TOGO_SBUF_COMMAND_TOO_BIG));
		togo_wt_send_cb(socket_item);
		socket_item->sstatus = 0;
		socket_item->rbytes = 0;
		return TRUE;
	}

	socket_item->rbytes -= (new_curr - socket_item->rcurr);
	socket_item->rcurr = new_curr;

	/* Dispatcher */
	togo_dispatcher(command_tag, socket_item, ntag);

	/* Send data */
	if (socket_item->sstatus != 1) {
		if (socket_item->sbuf == NULL || socket_item->ssize == 0) {
			if (socket_item->sstatus == 2) {
				togo_command_build_send(socket_item, TOGO_SBUF_FAIL,
						togo_strlen(TOGO_SBUF_FAIL));
			} else {
				togo_command_build_send(socket_item, TOGO_SBUF_OK,
						togo_strlen(TOGO_SBUF_OK));
			}
		}
		togo_wt_send_cb(socket_item);
	}
	socket_item->sstatus = 0;

	return TRUE;
}

BOOL togo_command_read_big_data(TOGO_THREAD_ITEM * socket_item,
		SEND_CALLBACK togo_wt_send_cb)
{
	size_t space;
	BDATA_CALLBACK callback;

	if (socket_item->bbuf == NULL) {
		return FALSE;
	}

	callback = socket_item->bcb;
	space = abs((socket_item->bbuf + socket_item->bsize) - socket_item->bcurr);
	if (space == 0) {
		togo_command_close_read_big_data(socket_item);
		return TRUE;
	}

	if (socket_item->rbytes >= space) {
		togo_memcpy(socket_item->bcurr, socket_item->rcurr, space);
		socket_item->rcurr = socket_item->rcurr + space;
		socket_item->rbytes = socket_item->rbytes - space;
		socket_item->bcurr = socket_item->bbuf + socket_item->bsize;

		if (callback != NULL) {
			callback(socket_item);
		}
		togo_command_close_read_big_data(socket_item);
	} else {
		/* There is no enough space, so we need to  continue to read the big data!*/

		togo_memcpy(socket_item->bcurr, socket_item->rcurr,
				socket_item->rbytes);
		socket_item->rcurr = socket_item->rcurr + socket_item->rbytes;
		socket_item->bcurr = socket_item->bcurr + socket_item->rbytes;
		socket_item->rbytes = 0;
		return TRUE;
	}

	/* Send data */
	if (socket_item->sstatus != 1) {
		if (socket_item->sbuf == NULL || socket_item->ssize == 0) {
			if (socket_item->sstatus == 2) {
				togo_command_build_send(socket_item, TOGO_SBUF_FAIL,
						togo_strlen(TOGO_SBUF_FAIL));
			} else {
				togo_command_build_send(socket_item, TOGO_SBUF_OK,
						togo_strlen(TOGO_SBUF_OK));
			}
		}
		togo_wt_send_cb(socket_item);
	}
	socket_item->sstatus = 0;

	return TRUE;
}

BOOL togo_command_read_big_data_skip(TOGO_THREAD_ITEM * socket_item)
{
	size_t space;
	space = abs(socket_item->bsize_skip - socket_item->bcurr_skip);
	if (space == 0) {
		socket_item->bsize_skip = 0;
		socket_item->bcurr_skip = 0;
		socket_item->rstatus = 0;
		return TRUE;
	}

	if (socket_item->rbytes >= space) {
		socket_item->rcurr = socket_item->rcurr + space;
		socket_item->rbytes = socket_item->rbytes - space;

		socket_item->bsize_skip = 0;
		socket_item->bcurr_skip = 0;
		socket_item->rstatus = 0;

	} else {
		socket_item->rcurr = socket_item->rcurr + space;
		socket_item->rbytes = 0;
	}
	return TRUE;
}

void togo_command_build_read(TOGO_THREAD_ITEM * socket_item, TOGO_POOL * bpool,
		u_char * buf, size_t len, BDATA_CALLBACK callback, void * param)
{
	socket_item->sstatus = 1;
	socket_item->rstatus = 1;

	socket_item->bparam = param;
	socket_item->bbuf = buf;
	socket_item->bcb = callback;
	socket_item->bcurr = buf;
	socket_item->bsize = len;
	socket_item->bpool = bpool;
}

void togo_command_build_read_skip(TOGO_THREAD_ITEM * socket_item, size_t len)
{
	socket_item->rstatus = 2;
	socket_item->bcurr_skip = 0;
	socket_item->bsize_skip = len;
}

void togo_command_build_send(TOGO_THREAD_ITEM * socket_item, u_char * buf,
		size_t len)
{
	socket_item->ssize = 0;
	if (socket_item->sbuf == NULL) {
		socket_item->sbuf = (u_char *) togo_pool_alloc(socket_item->worker_pool,
				TOGO_S_SBUF_INIT_SIZE);
		socket_item->sbuf_size = TOGO_S_SBUF_INIT_SIZE;
	}

	size_t sbuf_size = socket_item->sbuf_size;
	size_t new_buf_size = len + TOGO_S_SBUF_SPACE_SIZE;
	if (new_buf_size > sbuf_size) {

		/* Can only send data is less then TOGO_S_SBUF_MAX_SIZE*/
		if (new_buf_size >= TOGO_S_SBUF_MAX_SIZE) {
			togo_command_build_send(socket_item, TOGO_SBUF_TOO_BIG,
					togo_strlen(TOGO_SBUF_TOO_BIG));
			return;
		}

		socket_item->sbuf = togo_pool_realloc(socket_item->worker_pool,
				(void *) socket_item->sbuf, sbuf_size, new_buf_size);
		socket_item->sbuf_size = new_buf_size;
	}

	if (len > 0 && buf != NULL) {
		size_t start = togo_strlen(TOGO_SBUF_START);
		size_t end = togo_strlen(TOGO_SBUF_END);
		size_t total = start + len + end;
		togo_memcpy(socket_item->sbuf, TOGO_SBUF_START, start);
		togo_memcpy(socket_item->sbuf + start, buf, len);
		togo_memcpy(socket_item->sbuf + len + start, TOGO_SBUF_END, end);

		socket_item->ssize = total;
	}

}

void togo_command_build_send_fail(TOGO_THREAD_ITEM * socket_item)
{
	if (socket_item->sstatus == 0) {
		socket_item->sstatus = 2;
	}
}

void togo_command_build_send_null(TOGO_THREAD_ITEM * socket_item)
{
	togo_command_build_send(socket_item, TOGO_SBUF_NULL,
			togo_strlen(TOGO_SBUF_NULL));
	socket_item->sstatus = 0;
}

void togo_command_build_send_dbig(TOGO_THREAD_ITEM * socket_item, u_char * buf,
		size_t len, BDATA_CALLBACK callback, void * param)
{
	socket_item->sstatus = 3;

	socket_item->bsparam = param;
	socket_item->bscb = callback;
	socket_item->bssize = len;
	socket_item->bsbuf = buf;
}

static int togo_command_split(u_char *command, TOGO_COMMAND_TAG *command_tag)
{
	size_t len = togo_strlen(command);
	u_char * p;
	u_char * t;
	p = t = command;
	int x = 0;
	int i;
	int ntag = 0;

	for (i = 0; i < len; i++) {

		if (ntag == TOGO_COMMAND_TAG_MAX || ntag > TOGO_COMMAND_TAG_MAX) {
			break;
		}

		if (x == 0) {
			if (*p != ' ') {
				t = p;
				x = 1;
			}
		} else {
			if (*p == ' ') {
				*p = '\0';
				command_tag[ntag].value = t;
				command_tag[ntag].length = p - t;

				/* The command is too large */
				if (command_tag[ntag].length > TOGO_COMMAND_MAX_SIZE) {
					return -1;
				}

				ntag++;
				x = 0;
			}
		}

		p++;
	}

	if (p != t && ntag < TOGO_COMMAND_TAG_MAX) {
		int z = ntag - 1;
		if (command_tag[z].value != t) {
			command_tag[ntag].value = t;
			command_tag[ntag].length = p - t;

			/* The command is too large */
			if (command_tag[ntag].length > TOGO_COMMAND_MAX_SIZE) {
				return -1;
			}

			ntag++;
		}
	}

	return ntag;
}

static void togo_command_close_read_big_data(TOGO_THREAD_ITEM * socket_item)
{
	socket_item->rstatus = 0;

	socket_item->bbuf = NULL;
	socket_item->bcb = NULL;
	socket_item->bcurr = NULL;
	socket_item->bsize = 0;
	socket_item->bpool = NULL;
}

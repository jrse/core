/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "buffer.h"
#include "istream.h"
#include "ostream.h"
#include "str.h"
#include "message-send.h"
#include "message-size.h"
#include "imap-date.h"
#include "commands.h"
#include "imap-fetch.h"

#include <unistd.h>

const char *const *imap_fetch_get_body_fields(const char *fields)
{
	const char **field_list, **field, **dest;

	while (*fields == ' ')
		fields++;
	if (*fields == '(')
		fields++;

	field_list = t_strsplit(fields, " )");

	/* array ends at ")" element */
	for (field = dest = field_list; *field != NULL; field++) {
		if (strcmp(*field, ")") == 0)
			break;
		if (**field != '\0') {
			*dest = *field;
			dest++;
		}
	}
	*dest = NULL;

	return field_list;
}

static void fetch_uid(struct imap_fetch_context *ctx, struct mail *mail)
{
	str_printfa(ctx->str, "UID %u ", mail->uid);
}

static int fetch_flags(struct imap_fetch_context *ctx, struct mail *mail,
		       const struct mail_full_flags *flags)
{
	if (flags == NULL) {
		flags = mail->get_flags(mail);
		if (flags == NULL)
			return FALSE;
	}

	str_printfa(ctx->str, "FLAGS (%s) ", imap_write_flags(flags));
	return TRUE;
}

static int fetch_internaldate(struct imap_fetch_context *ctx, struct mail *mail)
{
	time_t time;

	time = mail->get_received_date(mail);
	if (time == (time_t)-1)
		return FALSE;

	str_printfa(ctx->str, "INTERNALDATE \"%s\" ", imap_to_datetime(time));
	return TRUE;
}

static int fetch_rfc822_size(struct imap_fetch_context *ctx, struct mail *mail)
{
	uoff_t size;

	size = mail->get_size(mail);
	if (size == (uoff_t)-1)
		return FALSE;

	str_printfa(ctx->str, "RFC822.SIZE %"PRIuUOFF_T" ", size);
	return TRUE;
}

static int fetch_body(struct imap_fetch_context *ctx, struct mail *mail)
{
	const char *body;

	body = mail->get_special(mail, MAIL_FETCH_IMAP_BODY);
	if (body == NULL)
		return FALSE;

	str_printfa(ctx->str, "BODY (%s) ", body);
	return TRUE;
}

static int fetch_bodystructure(struct imap_fetch_context *ctx,
			       struct mail *mail)
{
	const char *bodystructure;

	bodystructure = mail->get_special(mail, MAIL_FETCH_IMAP_BODYSTRUCTURE);
	if (bodystructure == NULL)
		return FALSE;

	str_printfa(ctx->str, "BODYSTRUCTURE (%s) ", bodystructure);
	return TRUE;
}

static int fetch_envelope(struct imap_fetch_context *ctx, struct mail *mail)
{
	const char *envelope;

	envelope = mail->get_special(mail, MAIL_FETCH_IMAP_ENVELOPE);
	if (envelope == NULL)
		return FALSE;

	str_printfa(ctx->str, "ENVELOPE (%s) ", envelope);
	return TRUE;
}

static int fetch_send_rfc822(struct imap_fetch_context *ctx, struct mail *mail)
{
	struct message_size hdr_size, body_size;
	struct istream *stream;
	const char *str;

	stream = mail->get_stream(mail, &hdr_size, &body_size);
	if (stream == NULL)
		return FALSE;

	message_size_add(&body_size, &hdr_size);

	str = t_strdup_printf(" RFC822 {%"PRIuUOFF_T"}\r\n",
			      body_size.virtual_size);
	if (ctx->first) {
		str++; ctx->first = FALSE;
	}
	if (o_stream_send_str(ctx->output, str) < 0)
		return FALSE;

	return message_send(ctx->output, stream, &body_size,
			    0, (uoff_t)-1, NULL, !mail->has_no_nuls) >= 0;
}

static int fetch_send_rfc822_header(struct imap_fetch_context *ctx,
				    struct mail *mail)
{
	struct message_size hdr_size;
	struct istream *stream;
	const char *str;

	stream = mail->get_stream(mail, &hdr_size, NULL);
	if (stream == NULL)
		return FALSE;

	str = t_strdup_printf(" RFC822.HEADER {%"PRIuUOFF_T"}\r\n",
			      hdr_size.virtual_size);
	if (ctx->first) {
		str++; ctx->first = FALSE;
	}
	if (o_stream_send_str(ctx->output, str) < 0)
		return FALSE;

	return message_send(ctx->output, stream, &hdr_size,
			    0, (uoff_t)-1, NULL, !mail->has_no_nuls) >= 0;
}

static int fetch_send_rfc822_text(struct imap_fetch_context *ctx,
				  struct mail *mail)
{
	struct message_size hdr_size, body_size;
	struct istream *stream;
	const char *str;

	stream = mail->get_stream(mail, &hdr_size, &body_size);
	if (stream == NULL)
		return FALSE;

	str = t_strdup_printf(" RFC822.TEXT {%"PRIuUOFF_T"}\r\n",
			      body_size.virtual_size);
	if (ctx->first) {
		str++; ctx->first = FALSE;
	}
	if (o_stream_send_str(ctx->output, str) < 0)
		return FALSE;

	i_stream_seek(stream, hdr_size.physical_size);
	return message_send(ctx->output, stream, &body_size,
			    0, (uoff_t)-1, NULL, !mail->has_no_nuls) >= 0;
}

static int fetch_mail(struct imap_fetch_context *ctx, struct mail *mail)
{
	const struct mail_full_flags *flags;
	struct imap_fetch_body_data *body;
	size_t len, orig_len;
	int failed, data_written, seen_updated = FALSE;

	if (!ctx->update_seen)
		flags = NULL;
	else {
		flags = mail->get_flags(mail);
		if (flags == NULL)
			return FALSE;

		if ((flags->flags & MAIL_SEEN) == 0) {
			if (!mail->update_flags(mail, &ctx->seen_flag,
						MODIFY_ADD))
				return FALSE;

			flags = NULL; /* \Seen won't update automatically */
			seen_updated = TRUE;
		}
	}

	t_push();

	str_truncate(ctx->str, 0);
	str_printfa(ctx->str, "* %u FETCH (", mail->seq);
	orig_len = str_len(ctx->str);

	failed = TRUE;
	data_written = FALSE;
	do {
		/* write the data into temp string */
		if (ctx->imap_data & IMAP_FETCH_UID)
			fetch_uid(ctx, mail);
		if ((ctx->fetch_data & MAIL_FETCH_FLAGS) || seen_updated)
			if (!fetch_flags(ctx, mail, flags))
				break;
		if (ctx->fetch_data & MAIL_FETCH_RECEIVED_DATE)
			if (!fetch_internaldate(ctx, mail))
				break;
		if (ctx->fetch_data & MAIL_FETCH_SIZE)
			if (!fetch_rfc822_size(ctx, mail))
				break;
		if (ctx->fetch_data & MAIL_FETCH_IMAP_BODY)
			if (!fetch_body(ctx, mail))
				break;
		if (ctx->fetch_data & MAIL_FETCH_IMAP_BODYSTRUCTURE)
			if (!fetch_bodystructure(ctx, mail))
				break;
		if (ctx->fetch_data & MAIL_FETCH_IMAP_ENVELOPE)
			if(!fetch_envelope(ctx, mail))
				break;

		/* send the data written into temp string */
		len = str_len(ctx->str);
		ctx->first = len == orig_len;

		if (!ctx->first)
			str_truncate(ctx->str, --len);
		if (o_stream_send(ctx->output, str_data(ctx->str), len) < 0)
			break;

		data_written = TRUE;

		/* large data */
		if (ctx->imap_data & IMAP_FETCH_RFC822)
			if (!fetch_send_rfc822(ctx, mail))
				break;
		if (ctx->imap_data & IMAP_FETCH_RFC822_HEADER)
			if (!fetch_send_rfc822_header(ctx, mail))
				break;
		if (ctx->imap_data & IMAP_FETCH_RFC822_TEXT)
			if (!fetch_send_rfc822_text(ctx, mail))
				break;

		for (body = ctx->bodies; body != NULL; body = body->next) {
			if (!imap_fetch_body_section(ctx, body, mail))
				break;
		}

		failed = FALSE;
	} while (0);

	if (data_written) {
		if (o_stream_send(ctx->output, ")\r\n", 3) < 0)
			failed = TRUE;
	}

	t_pop();
	return !failed;
}

int imap_fetch(struct client *client,
	       enum mail_fetch_field fetch_data,
	       enum imap_fetch_field imap_data,
	       struct imap_fetch_body_data *bodies,
	       const char *messageset, int uidset)
{
	struct mailbox *box = client->mailbox;
	struct imap_fetch_context ctx;
	struct mail *mail;
	struct imap_fetch_body_data *body;
	const char *null = NULL;
	const char *const *wanted_headers, *const *arr;
	buffer_t *buffer;
	int all_found;

	memset(&ctx, 0, sizeof(ctx));
	ctx.fetch_data = fetch_data;
	ctx.imap_data = imap_data;
	ctx.bodies = bodies;
	ctx.output = client->output;
	ctx.select_counter = client->select_counter;
	ctx.seen_flag.flags = MAIL_SEEN;

	if (!box->is_readonly(box)) {
		/* If we have any BODY[..] sections, \Seen flag is added for
		   all messages. */
		for (body = bodies; body != NULL; body = body->next) {
			if (!body->peek) {
				ctx.update_seen = TRUE;
				break;
			}
		}

		if (imap_data & (IMAP_FETCH_RFC822|IMAP_FETCH_RFC822_TEXT))
			ctx.update_seen = TRUE;
	}

	/* If we have only BODY[HEADER.FIELDS (...)] fetches, get them
	   separately rather than parsing the full header so mail storage
	   can try to cache them. */
	ctx.body_fetch_from_cache = TRUE;
	buffer = buffer_create_dynamic(data_stack_pool, 64, (size_t)-1);
	for (body = bodies; body != NULL; body = body->next) {
		if (strncmp(body->section, "HEADER.FIELDS ", 14) != 0) {
                        ctx.body_fetch_from_cache = FALSE;
			break;
		}

		arr = imap_fetch_get_body_fields(body->section + 14);
		while (*arr != NULL) {
			buffer_append(buffer, arr, sizeof(*arr));
			arr++;
		}
	}
	buffer_append(buffer, &null, sizeof(null));
	wanted_headers = !ctx.body_fetch_from_cache ? NULL :
		buffer_get_data(buffer, NULL);

	if (ctx.update_seen) {
		if (!box->lock(box, MAILBOX_LOCK_FLAGS | MAILBOX_LOCK_READ))
			return -1;
	}

	ctx.fetch_ctx = box->fetch_init(box, fetch_data, wanted_headers,
					messageset, uidset);
	if (ctx.fetch_ctx == NULL)
		ctx.failed = TRUE;
	else {
		ctx.str = str_new(default_pool, 8192);
		while ((mail = box->fetch_next(ctx.fetch_ctx)) != NULL) {
			if (!fetch_mail(&ctx, mail)) {
				ctx.failed = TRUE;
				break;
			}
		}
		str_free(ctx.str);

		if (!box->fetch_deinit(ctx.fetch_ctx, &all_found))
			ctx.failed = TRUE;
	}

	(void)box->lock(box, MAILBOX_LOCK_UNLOCK);

	return ctx.failed ? -1 : all_found;
}

/*
 * Copyright (c) 2019 Tim van der Molen <tim@kariliq.nl>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sigbak.h"

#define FORMAT_MAILDIR	0
#define FORMAT_TEXT	1

static void
maildir_create(const char *path)
{
	int fd;

	if (mkdir(path, 0777) == -1)
		err(1, "mkdir: %s", path);

	if ((fd = open(path, O_RDONLY | O_DIRECTORY)) == -1)
		err(1, "open: %s", path);

	if (mkdirat(fd, "cur", 0777) == -1)
		err(1, "mkdirat: %s/%s", path, "cur");

	if (mkdirat(fd, "new", 0777) == -1)
		err(1, "mkdirat: %s/%s", path, "new");

	if (mkdirat(fd, "tmp", 0777) == -1)
		err(1, "mkdirat: %s/%s", path, "tmp");

	close(fd);
}

static FILE *
maildir_open_file(const char *maildir, int64_t date_recv, int64_t date_sent)
{
	FILE	*fp;
	char	*path;

	/* Intentionally create deterministic filenames */
	/* XXX Shouldn't write directly into cur */
	if (asprintf(&path, "%s/cur/%" PRId64 ".%" PRId64 ".localhost:2,S",
	    maildir, date_recv, date_sent) == -1) {
		warnx("asprintf() failed");
		return NULL;
	}

	if ((fp = fopen(path, "wx")) == NULL)
		warn("fopen: %s", path);

	free(path);
	return fp;
}

static void
maildir_write_address_header(FILE *fp, const char *hdr, const char *addr,
    const char *name)
{
	if (name == NULL)
		fprintf(fp, "%s: %s@invalid\n", hdr, addr);
	else
		/* XXX Need to escape double quotes in name */
		fprintf(fp, "%s: \"%s\" <%s@invalid>\n", hdr, name, addr);
}

static void
maildir_write_date_header(FILE *fp, const char *hdr, int64_t date)
{
	const char	*days[] = {
	    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

	const char	*months[] = {
	    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
	    "Oct", "Nov", "Dec" };

	struct tm	*tm;
	time_t		 tt;

	tt = date / 1000;

	if ((tm = localtime(&tt)) == NULL) {
		warnx("localtime() failed");
		return;
	}

	fprintf(fp, "%s: %s, %d %s %d %02d:%02d:%02d %c%02ld%02ld\n",
	    hdr,
	    days[tm->tm_wday],
	    tm->tm_mday,
	    months[tm->tm_mon],
	    tm->tm_year + 1900,
	    tm->tm_hour,
	    tm->tm_min,
	    tm->tm_sec,
	    (tm->tm_gmtoff < 0) ? '-' : '+',
	    labs(tm->tm_gmtoff) / 3600,
	    labs(tm->tm_gmtoff) % 3600 / 60);
}

static int
maildir_write_mms(struct sbk_ctx *ctx, const char *maildir,
    struct sbk_mms *mms)
{
	FILE	*fp;
	char	*name, *phone;
	int	 isgroup, ret;

	ret = -1;
	isgroup = sbk_is_group(ctx, mms->address);

	if (isgroup) {
		if (sbk_get_group(ctx, mms->address, &name) == -1) {
			warnx("%s", sbk_error(ctx));
			return -1;
		}
		phone = NULL;
	} else {
		if (sbk_get_contact(ctx, mms->address, &name, &phone) == -1) {
			warnx("%s", sbk_error(ctx));
			return -1;
		}
	}

	if (sbk_get_long_message(ctx, mms) == -1) {
		warnx("%s", sbk_error(ctx));
		goto out;
	}

	if ((fp = maildir_open_file(maildir, mms->date_recv, mms->date_sent))
	    == NULL)
		goto out;

	if (SBK_IS_OUTGOING_MESSAGE(mms->type)) {
		maildir_write_address_header(fp, "From", "you", "You");
		maildir_write_address_header(fp, "To",
		    isgroup ? "group" : phone, name);
	} else {
		maildir_write_address_header(fp, "From", phone, name);
		maildir_write_address_header(fp, "To", "you", "You");
	}

	maildir_write_date_header(fp, "Date", mms->date_sent);

	if (!SBK_IS_OUTGOING_MESSAGE(mms->type))
		maildir_write_date_header(fp, "X-Received", mms->date_recv);

	fprintf(fp, "X-Thread: %d\n", mms->thread);
	fputs("MIME-Version: 1.0\n", fp);
	fputs("Content-Type: text/plain; charset=utf-8\n", fp);
	fputs("Content-Disposition: inline\n", fp);

	if (mms->body != NULL)
		fprintf(fp, "\n%s\n", mms->body);

	fclose(fp);
	ret = 0;

out:
	freezero_string(name);
	freezero_string(phone);
	return ret;
}

static int
maildir_write_sms(struct sbk_ctx *ctx, const char *maildir,
    struct sbk_sms *sms)
{
	FILE	*fp;
	char	*name, *phone;
	int	 ret;

	ret = -1;

	if (sbk_get_contact(ctx, sms->address, &name, &phone) == -1) {
		warnx("%s", sbk_error(ctx));
		return -1;
	}

	if ((fp = maildir_open_file(maildir, sms->date_recv, sms->date_sent))
	    == NULL)
		goto out;

	if (SBK_IS_OUTGOING_MESSAGE(sms->type)) {
		maildir_write_address_header(fp, "From", "you", "You");
		maildir_write_address_header(fp, "To",
		    (phone != NULL) ? phone : "unknown",
		    (name != NULL) ? name : "unknown");
	} else {
		maildir_write_address_header(fp, "From",
		    (phone != NULL) ? phone : "unknown",
		    (name != NULL) ? name : "unknown");
		maildir_write_address_header(fp, "To", "you", "You");
	}

	maildir_write_date_header(fp, "Date", sms->date_sent);

	if (!SBK_IS_OUTGOING_MESSAGE(sms->type))
		maildir_write_date_header(fp, "X-Received", sms->date_recv);

	fprintf(fp, "X-Thread: %d\n", sms->thread);
	fputs("MIME-Version: 1.0\n", fp);
	fputs("Content-Type: text/plain; charset=utf-8\n", fp);
	fputs("Content-Disposition: inline\n", fp);

	if (sms->body != NULL)
		fprintf(fp, "\n%s\n", sms->body);

	fclose(fp);
	ret = 0;

out:
	freezero_string(name);
	freezero_string(phone);
	return ret;
}

static int
maildir_write_messages(struct sbk_ctx *ctx, const char *maildir, int thread)
{
	struct sbk_mms_list	*mmslst;
	struct sbk_sms_list	*smslst;
	struct sbk_mms		*mms;
	struct sbk_sms		*sms;
	int			 ret;

	if ((mmslst = sbk_get_mmses(ctx, thread)) == NULL) {
		warnx("Cannot get mms messages: %s", sbk_error(ctx));
		return -1;
	}

	if ((smslst = sbk_get_smses(ctx, thread)) == NULL) {
		warnx("Cannot get sms messages: %s", sbk_error(ctx));
		sbk_free_mms_list(mmslst);
		return -1;
	}

	mms = SIMPLEQ_FIRST(mmslst);
	sms = SIMPLEQ_FIRST(smslst);
	ret = 0;

	/* Print mms and sms messages in the order they were received */
	for (;;)
		if (mms == NULL) {
			if (sms == NULL)
				break; /* Done */
			else {
				ret |= maildir_write_sms(ctx, maildir, sms);
				sms = SIMPLEQ_NEXT(sms, entries);
			}
		} else {
			if (sms == NULL || mms->date_recv < sms->date_recv) {
				ret |= maildir_write_mms(ctx, maildir, mms);
				mms = SIMPLEQ_NEXT(mms, entries);
			} else {
				ret |= maildir_write_sms(ctx, maildir, sms);
				sms = SIMPLEQ_NEXT(sms, entries);
			}
		}

	sbk_free_mms_list(mmslst);
	sbk_free_sms_list(smslst);
	return (ret == 0) ? 0 : -1;
}

static int
text_write_mms(struct sbk_ctx *ctx, FILE *fp, struct sbk_mms *mms)
{
	struct sbk_attachment *att;
	char	*name, *phone;
	int	 isgroup;

	isgroup = sbk_is_group(ctx, mms->address);

	if (isgroup) {
		if (sbk_get_group(ctx, mms->address, &name) == -1) {
			warnx("%s", sbk_error(ctx));
			return -1;
		}
		phone = NULL;
	} else {
		if (sbk_get_contact(ctx, mms->address, &name, &phone) == -1) {
			warnx("%s", sbk_error(ctx));
			return -1;
		}
	}

	if (SBK_IS_OUTGOING_MESSAGE(mms->type))
		fputs("To: ", fp);
	else
		fputs("From: ", fp);

	fprintf(fp, "%s (%s)\n",
	    (name != NULL) ? name : "unknown",
	    isgroup ? "group" : ((phone != NULL) ? phone : "unknown"));

	maildir_write_date_header(fp, "Sent", mms->date_sent);

	if (!SBK_IS_OUTGOING_MESSAGE(mms->type))
		maildir_write_date_header(fp, "Received", mms->date_recv);

	fprintf(fp, "Thread: %d\n", mms->thread);

	if (mms->nattachments > 0) {
		if (sbk_get_attachments(ctx, mms) == -1) {
			warnx("%s", sbk_error(ctx));
			return -1;
		}

		SIMPLEQ_FOREACH(att, mms->attachments, entries) {
			fputs("Attachment: ", fp);
			if (att->filename == NULL)
				fputs("no filename", fp);
			else
				fprintf(fp, "\"%s\"", att->filename);
			fprintf(fp, " (%s, %" PRIu64 " bytes, id %" PRId64
			    "-%" PRId64 ")\n", (att->content_type != NULL) ?
			    att->content_type : "", att->size, att->rowid,
			    att->attachmentid);
		}
	}

	if (sbk_get_long_message(ctx, mms) == -1) {
		warnx("%s", sbk_error(ctx));
		return -1;
	}

	if (mms->body != NULL)
		fprintf(fp, "\n%s\n\n", mms->body);
	else
		fputs("\n", fp);

	freezero_string(name);
	freezero_string(phone);

	return 0;
}

static int
text_write_sms(struct sbk_ctx *ctx, FILE *fp, struct sbk_sms *sms)
{
	char *name, *phone;

	if (sbk_get_contact(ctx, sms->address, &name, &phone) == -1) {
		warnx("%s", sbk_error(ctx));
		return -1;
	}

	if (SBK_IS_OUTGOING_MESSAGE(sms->type))
		fputs("To: ", fp);
	else
		fputs("From: ", fp);

	fprintf(fp, "%s (%s)\n",
	    (name != NULL) ? name : "unknown",
	    (phone != NULL) ? phone : "unknown");

	maildir_write_date_header(fp, "Sent", sms->date_sent);

	if (!SBK_IS_OUTGOING_MESSAGE(sms->type))
		maildir_write_date_header(fp, "Received", sms->date_recv);

	fprintf(fp, "Thread: %d\n", sms->thread);

	if (sms->body != NULL)
		fprintf(fp, "\n%s\n\n", sms->body);
	else
		fputs("\n", fp);

	freezero_string(name);
	freezero_string(phone);
	return 0;
}

static int
text_write_messages(struct sbk_ctx *ctx, const char *outfile, int thread)
{
	struct sbk_mms_list	*mmslst;
	struct sbk_sms_list	*smslst;
	struct sbk_mms		*mms;
	struct sbk_sms		*sms;
	FILE			*fp;
	int			 ret;

	if (outfile == NULL)
		fp = stdout;
	else if ((fp = fopen(outfile, "wx")) == NULL) {
		warn("fopen: %s", outfile);
		return -1;
	}

	if ((mmslst = sbk_get_mmses(ctx, thread)) == NULL) {
		warnx("Cannot get mms messages: %s", sbk_error(ctx));
		return -1;
	}

	if ((smslst = sbk_get_smses(ctx, thread)) == NULL) {
		warnx("Cannot get sms messages: %s", sbk_error(ctx));
		sbk_free_mms_list(mmslst);
		return -1;
	}

	mms = SIMPLEQ_FIRST(mmslst);
	sms = SIMPLEQ_FIRST(smslst);
	ret = 0;

	/* Print mms and sms messages in the order they were received */
	for (;;)
		if (mms == NULL) {
			if (sms == NULL)
				break; /* Done */
			else {
				ret |= text_write_sms(ctx, fp, sms);
				sms = SIMPLEQ_NEXT(sms, entries);
			}
		} else {
			if (sms == NULL || mms->date_recv < sms->date_recv) {
				ret |= text_write_mms(ctx, fp, mms);
				mms = SIMPLEQ_NEXT(mms, entries);
			} else {
				ret |= text_write_sms(ctx, fp, sms);
				sms = SIMPLEQ_NEXT(sms, entries);
			}
		}

	sbk_free_mms_list(mmslst);
	sbk_free_sms_list(smslst);

	if (fp != stdout)
		fclose(fp);

	return (ret == 0) ? 0 : -1;
}

int
cmd_messages(int argc, char **argv)
{
	struct sbk_ctx	*ctx;
	char		*dest, *passfile, passphr[128];
	const char	*errstr;
	int		 c, format, ret, thread;

	format = FORMAT_TEXT;
	passfile = NULL;
	thread = -1;

	while ((c = getopt(argc, argv, "f:p:t:")) != -1)
		switch (c) {
		case 'f':
			if (strcmp(optarg, "maildir") == 0)
				format = FORMAT_MAILDIR;
			else if (strcmp(optarg, "text") == 0)
				format = FORMAT_TEXT;
			else
				errx(1, "%s: invalid format", optarg);
			break;
		case 'p':
			passfile = optarg;
			break;
		case 't':
			errstr = NULL;
			thread = strtonum(optarg, 1, INT_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "%s: thread id is %s", optarg,
				    errstr);
			break;
		default:
			goto usage;
		}

	argc -= optind;
	argv += optind;

	switch (argc) {
	case 1:
		if (format == FORMAT_MAILDIR)
			goto usage;
		dest = NULL;
		break;
	case 2:
		dest = argv[1];
		if (format == FORMAT_MAILDIR)
			maildir_create(dest);
		if (unveil(dest, "wc") == -1)
			err(1, "unveil");
		break;
	default:
		goto usage;
	}

	if (unveil(argv[0], "r") == -1)
		err(1, "unveil");

	/* For SQLite */
	if (unveil("/dev/urandom", "r") == -1)
		err(1, "unveil");

	if (passfile == NULL) {
		if (pledge("stdio rpath wpath cpath tty", NULL) == -1)
			err(1, "pledge");
	} else {
		if (unveil(passfile, "r") == -1)
			err(1, "unveil");

		if (pledge("stdio rpath wpath cpath", NULL) == -1)
			err(1, "pledge");
	}

	if ((ctx = sbk_ctx_new()) == NULL)
		errx(1, "Cannot create backup context");

	if (get_passphrase(passfile, passphr, sizeof passphr) == -1)
		return -1;

	if (sbk_open(ctx, argv[0], passphr) == -1) {
		warnx("%s: %s", argv[0], sbk_error(ctx));
		explicit_bzero(passphr, sizeof passphr);
		sbk_ctx_free(ctx);
		return 1;
	}

	explicit_bzero(passphr, sizeof passphr);

	if (passfile == NULL && pledge("stdio rpath wpath cpath", NULL) == -1)
		err(1, "pledge");

	switch (format) {
	case FORMAT_MAILDIR:
		ret = maildir_write_messages(ctx, dest, thread);
		break;
	case FORMAT_TEXT:
		ret = text_write_messages(ctx, dest, thread);
		break;
	}

	sbk_close(ctx);
	sbk_ctx_free(ctx);
	return (ret == 0) ? 0 : 1;

usage:
	usage("messages", "[-f format] [-p passfile] [-t thread] backup dest");
}

/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "mail-index.h"
#include "mail-index-util.h"
#include "mail-custom-flags.h"
#include "index-storage.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* How many seconds to keep index opened for reuse after it's been closed */
#define INDEX_CACHE_TIMEOUT 10
/* How many closed indexes to keep */
#define INDEX_CACHE_MAX 3

#define LOCK_NOTIFY_INTERVAL 30

struct index_list {
	struct index_list *next;

	struct mail_index *index;
	int refcount;

	time_t destroy_time;
};

static struct index_list *indexes = NULL;
static struct timeout *to_index = NULL;
static int index_storage_refcount = 0;

void index_storage_init(struct mail_storage *storage __attr_unused__)
{
	index_storage_refcount++;
}

void index_storage_deinit(struct mail_storage *storage __attr_unused__)
{
	if (--index_storage_refcount > 0)
		return;

        index_storage_destroy_unrefed();
}

void index_storage_add(struct mail_index *index)
{
	struct index_list *list;

	list = i_new(struct index_list, 1);
	list->refcount = 1;
	list->index = index;

	list->next = indexes;
	indexes = list;
}

struct mail_index *index_storage_lookup_ref(const char *path)
{
	struct index_list **list, *rec;
	struct mail_index *match;
	struct stat st1, st2;
	int destroy_count;

	if (stat(path, &st1) < 0)
		return NULL;

	/* compare inodes so we don't break even with symlinks */
	destroy_count = 0; match = NULL;
	for (list = &indexes; *list != NULL;) {
		rec = *list;

		if (stat(rec->index->dir, &st2) == 0) {
			if (st1.st_ino == st2.st_ino &&
			    st1.st_dev == st2.st_dev) {
				rec->refcount++;
				match = rec->index;
			}
		}

		if (rec->refcount == 0) {
			if (rec->destroy_time <= ioloop_time ||
			    destroy_count >= INDEX_CACHE_MAX) {
				rec->index->free(rec->index);
				*list = rec->next;
				i_free(rec);
				continue;
			} else {
				destroy_count++;
			}
		}

                list = &(*list)->next;
	}

	return match;
}

static void destroy_unrefed(int all)
{
	struct index_list **list, *rec;

	for (list = &indexes; *list != NULL;) {
		rec = *list;

		if (rec->refcount == 0 &&
		    (all || rec->destroy_time <= ioloop_time)) {
			rec->index->free(rec->index);
			*list = rec->next;
			i_free(rec);
		} else {
			list = &(*list)->next;
		}
	}

	if (indexes == NULL && to_index != NULL) {
		timeout_remove(to_index);
		to_index = NULL;
	}
}

static void index_removal_timeout(void *context __attr_unused__)
{
	destroy_unrefed(FALSE);
}

void index_storage_unref(struct mail_index *index)
{
	struct index_list *list;

	for (list = indexes; list != NULL; list = list->next) {
		if (list->index == index)
			break;
	}

	i_assert(list != NULL);
	i_assert(list->refcount > 0);

	list->refcount--;
	list->destroy_time = ioloop_time + INDEX_CACHE_TIMEOUT;
	if (to_index == NULL)
		to_index = timeout_add(1000, index_removal_timeout, NULL);
}

void index_storage_destroy_unrefed(void)
{
	destroy_unrefed(TRUE);
}

static enum mail_cache_field get_cache_fields(const char *fields)
{
	static enum mail_cache_field field_masks[] = {
		MAIL_CACHE_SENT_DATE,
		MAIL_CACHE_RECEIVED_DATE,
		MAIL_CACHE_VIRTUAL_FULL_SIZE,
		MAIL_CACHE_BODY,
		MAIL_CACHE_BODYSTRUCTURE,
		MAIL_CACHE_MESSAGEPART,
	};
	static const char *field_names[] = {
		"sent_date",
		"received_date",
		"virtual_size",
		"body",
		"bodystructure",
		"messagepart",
		NULL
	};

	const char *const *arr;
	enum mail_cache_field ret;
	int i;

	if (fields == NULL || *fields == '\0')
		return 0;

	ret = 0;
	for (arr = t_strsplit(fields, " ,"); *arr != NULL; arr++) {
		if (*arr == '\0')
			continue;

		for (i = 0; field_names[i] != NULL; i++) {
			if (strcasecmp(field_names[i], *arr) == 0) {
				ret |= field_masks[i];
				break;
			}
		}
		if (field_names[i] == NULL) {
			i_error("Invalid cache field name '%s', ignoring ",
				*arr);
		}
	}

	return ret;
}

static enum mail_cache_field get_default_cache_fields(void)
{
	static enum mail_cache_field ret = 0;
	static int ret_set = FALSE;

	if (ret_set)
		return ret;

	ret = get_cache_fields(getenv("MAIL_CACHE_FIELDS"));
	ret_set = TRUE;
	return ret;
}

static enum mail_cache_field get_never_cache_fields(void)
{
	static enum mail_cache_field ret = 0;
	static int ret_set = FALSE;

	if (ret_set)
		return ret;

	ret = get_cache_fields(getenv("MAIL_NEVER_CACHE_FIELDS"));
	ret_set = TRUE;
	return ret;
}

static void lock_notify(enum mail_lock_notify_type notify_type,
			unsigned int secs_left, void *context)
{
	struct index_mailbox *ibox = context;
	struct mail_storage *storage = ibox->box.storage;
	const char *str;
	time_t now;

	if ((secs_left % 15) != 0) {
		/* update alarm() so that we get back here around the same
		   time we want the next notify. also try to use somewhat
		   rounded times. this affects only fcntl() locking, dotlock
		   and flock() calls should be calling us constantly */
		alarm(secs_left%15);
	}

	/* if notify type changes, print the message immediately */
	now = time(NULL);
	if (ibox->last_notify_type == (enum mail_lock_notify_type)-1 ||
	    ibox->last_notify_type == notify_type) {
		if (ibox->last_notify_type == (enum mail_lock_notify_type)-1 &&
		    notify_type == MAIL_LOCK_NOTIFY_MAILBOX_OVERRIDE) {
			/* first override notification, show it */
		} else {
			if (now < ibox->next_lock_notify || secs_left < 15)
				return;
		}
	}

	ibox->next_lock_notify = now + LOCK_NOTIFY_INTERVAL;
        ibox->last_notify_type = notify_type;

	switch (notify_type) {
	case MAIL_LOCK_NOTIFY_MAILBOX_ABORT:
		str = t_strdup_printf("Mailbox is locked, will abort in "
				      "%u seconds", secs_left);
		storage->callbacks->notify_no(&ibox->box, str,
					      storage->callback_context);
		break;
	case MAIL_LOCK_NOTIFY_MAILBOX_OVERRIDE:
		str = t_strdup_printf("Stale mailbox lock file detected, "
				      "will override in %u seconds", secs_left);
		storage->callbacks->notify_ok(&ibox->box, str,
					      storage->callback_context);
		break;
	case MAIL_LOCK_NOTIFY_INDEX_ABORT:
		str = t_strdup_printf("Mailbox index is locked, will abort in "
				      "%u seconds", secs_left);
		storage->callbacks->notify_no(&ibox->box, str,
					      storage->callback_context);
		break;
	}
}

void index_storage_init_lock_notify(struct index_mailbox *ibox)
{
	if (ibox->index->mailbox_readonly)
		ibox->readonly = TRUE;

	ibox->next_lock_notify = time(NULL) + LOCK_NOTIFY_INTERVAL;
	ibox->last_notify_type = (enum mail_lock_notify_type)-1;

	ibox->index->set_lock_notify_callback(ibox->index, lock_notify, ibox);
}

int index_storage_lock(struct index_mailbox *ibox,
		       enum mail_lock_type lock_type)
{
	int ret = TRUE;

	if (lock_type == MAIL_LOCK_UNLOCK) {
		if (ibox->trans_ctx != NULL) {
			if (!mail_cache_transaction_commit(ibox->trans_ctx))
				ret = FALSE;
			if (!mail_cache_transaction_end(ibox->trans_ctx))
				ret = FALSE;
			ibox->trans_ctx = NULL;
		}
		if (ibox->lock_type != MAILBOX_LOCK_UNLOCK)
			return TRUE;
	} else {
		if (ibox->lock_type == MAIL_LOCK_EXCLUSIVE)
			return TRUE;
	}

	/* we have to set/reset this every time, because the same index
	   may be used by multiple IndexMailboxes. */
        index_storage_init_lock_notify(ibox);
	if (!ibox->index->set_lock(ibox->index, lock_type))
		ret = FALSE;
	ibox->index->set_lock_notify_callback(ibox->index, NULL, NULL);

	if (!ret)
		return mail_storage_set_index_error(ibox);

	return TRUE;
}

struct index_mailbox *
index_storage_mailbox_init(struct mail_storage *storage, struct mailbox *box,
			   struct mail_index *index, const char *name,
			   enum mailbox_open_flags flags)
{
	struct index_mailbox *ibox;
	enum mail_index_open_flags index_flags;

	i_assert(name != NULL);

	index_flags = MAIL_INDEX_OPEN_FLAG_CREATE;
	if ((flags & MAILBOX_OPEN_FAST) != 0)
		index_flags |= MAIL_INDEX_OPEN_FLAG_FAST;
	if ((flags & MAILBOX_OPEN_READONLY) != 0)
		index_flags |= MAIL_INDEX_OPEN_FLAG_UPDATE_RECENT;
	if ((flags & MAILBOX_OPEN_MMAP_INVALIDATE) != 0)
		index_flags |= MAIL_INDEX_OPEN_FLAG_MMAP_INVALIDATE;

	do {
		ibox = i_new(struct index_mailbox, 1);
		ibox->box = *box;

		ibox->box.storage = storage;
		ibox->box.name = i_strdup(name);
		ibox->readonly = (flags & MAILBOX_OPEN_READONLY) != 0;

		ibox->index = index;

		ibox->next_lock_notify = time(NULL) + LOCK_NOTIFY_INTERVAL;
		index->set_lock_notify_callback(index, lock_notify, ibox);

		if (!index->opened) {
			/* open the index first */
			if (!index->open(index, index_flags))
				break;

			mail_cache_set_defaults(index->cache,
						get_default_cache_fields(),
						get_never_cache_fields());

			if (INDEX_IS_IN_MEMORY(index) &&
			    storage->index_dir != NULL) {
				storage->callbacks->notify_no(&ibox->box,
					"Couldn't use index files",
					storage->callback_context);
			}
		}

		if (!ibox->index->set_lock(ibox->index, MAIL_LOCK_SHARED))
			break;

		ibox->synced_messages_count =
			mail_index_get_header(index)->messages_count;

		if (!ibox->index->set_lock(ibox->index, MAIL_LOCK_UNLOCK))
			break;

		index->set_lock_notify_callback(index, NULL, NULL);

		return ibox;
	} while (0);

	mail_storage_set_index_error(ibox);
	index_storage_mailbox_free(&ibox->box);
	return NULL;
}

int index_storage_mailbox_free(struct mailbox *box)
{
	struct index_mailbox *ibox = (struct index_mailbox *) box;

	/* make sure we're unlocked */
	(void)ibox->index->set_lock(ibox->index, MAIL_LOCK_UNLOCK);

	index_mailbox_check_remove_all(ibox);
	if (ibox->index != NULL)
		index_storage_unref(ibox->index);

	i_free(box->name);
	i_free(box);

	return TRUE;
}

int index_storage_is_readonly(struct mailbox *box)
{
	struct index_mailbox *ibox = (struct index_mailbox *) box;

	return ibox->readonly;
}

int index_storage_allow_new_custom_flags(struct mailbox *box)
{
	struct index_mailbox *ibox = (struct index_mailbox *) box;

	return ibox->index->allow_new_custom_flags;
}

int index_storage_is_inconsistency_error(struct mailbox *box)
{
	struct index_mailbox *ibox = (struct index_mailbox *) box;

	return ibox->inconsistent;
}

void index_storage_set_callbacks(struct mail_storage *storage,
				 struct mail_storage_callbacks *callbacks,
				 void *context)
{
	memcpy(storage->callbacks, callbacks,
	       sizeof(struct mail_storage_callbacks));
	storage->callback_context = context;
}

int mail_storage_set_index_error(struct index_mailbox *ibox)
{
	switch (ibox->index->get_last_error(ibox->index)) {
	case MAIL_INDEX_ERROR_NONE:
	case MAIL_INDEX_ERROR_INTERNAL:
		mail_storage_set_internal_error(ibox->box.storage);
		break;
	case MAIL_INDEX_ERROR_INCONSISTENT:
		ibox->inconsistent = TRUE;
		break;
	case MAIL_INDEX_ERROR_DISKSPACE:
		mail_storage_set_error(ibox->box.storage, "Out of disk space");
		break;
	case MAIL_INDEX_ERROR_INDEX_LOCK_TIMEOUT:
		mail_storage_set_error(ibox->box.storage,
			"Timeout while waiting for lock to index of mailbox %s",
			ibox->box.name);
		break;
	case MAIL_INDEX_ERROR_MAILBOX_LOCK_TIMEOUT:
		mail_storage_set_error(ibox->box.storage,
			"Timeout while waiting for lock to mailbox %s",
			ibox->box.name);
		break;
	}

	index_reset_error(ibox->index);
	return FALSE;
}

int index_mailbox_fix_custom_flags(struct index_mailbox *ibox,
				   enum mail_flags *flags,
				   const char *custom_flags[],
				   unsigned int custom_flags_count)
{
	int ret;

	ret = mail_custom_flags_fix_list(ibox->index->custom_flags,
					 flags, custom_flags,
					 custom_flags_count);
	switch (ret) {
	case 1:
		return TRUE;
	case 0:
		mail_storage_set_error(ibox->box.storage,
			"Maximum number of different custom flags exceeded");
		return FALSE;
	default:
		return mail_storage_set_index_error(ibox);
	}
}

unsigned int index_storage_get_recent_count(struct mail_index *index)
{
	struct mail_index_header *hdr;
	struct mail_index_record *rec;
	unsigned int seq;

	hdr = mail_index_get_header(index);
	if (index->first_recent_uid <= 1) {
		/* all are recent */
		return hdr->messages_count;
	}

	/* get the first recent message */
	if (index->first_recent_uid >= hdr->next_uid)
		return 0;

	rec = index->lookup_uid_range(index, index->first_recent_uid,
				      hdr->next_uid - 1, &seq);
	return rec == NULL ? 0 : hdr->messages_count+1 - seq;
}

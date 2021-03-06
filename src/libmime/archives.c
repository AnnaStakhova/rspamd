/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include "message.h"
#include "task.h"
#include "archives.h"

static void
rspamd_archive_dtor (gpointer p)
{
	struct rspamd_archive *arch = p;
	struct rspamd_archive_file *f;
	guint i;

	for (i = 0; i < arch->files->len; i ++) {
		f = g_ptr_array_index (arch->files, i);

		if (f->fname) {
			g_string_free (f->fname, TRUE);
		}
		g_slice_free1 (sizeof (*f), f);
	}

	g_ptr_array_free (arch->files, TRUE);
}

static void
rspamd_archive_process_zip (struct rspamd_task *task,
		struct rspamd_mime_part *part)
{
	const guchar *p, *start, *end, *eocd = NULL, *cd;
	const guint32 eocd_magic = 0x06054b50, cd_basic_len = 46;
	const guchar cd_magic[] = {0x50, 0x4b, 0x01, 0x02};
	const guint max_processed = 1024;
	guint32 cd_offset, cd_size, comp_size, uncomp_size, processed = 0;
	guint16 extra_len, fname_len, comment_len;
	struct rspamd_archive *arch;
	struct rspamd_archive_file *f;

	/* Zip files have interesting data at the end of archive */
	p = part->parsed_data.begin + part->parsed_data.len - 1;
	start = part->parsed_data.begin;
	end = p;

	/* Search for EOCD:
	 * 22 bytes is a typical size of eocd without a comment and
	 * end points one byte after the last character
	 */
	p -= 21;

	while (p > start + sizeof (guint32)) {
		guint32 t;

		if (processed > max_processed) {
			break;
		}

		/* XXX: not an efficient approach */
		memcpy (&t, p, sizeof (t));

		if (GUINT32_FROM_LE (t) == eocd_magic) {
			eocd = p;
			break;
		}

		p --;
		processed ++;
	}


	if (eocd == NULL) {
		/* Not a zip file */
		msg_debug_task ("zip archive is invalid (no EOCD)");

		return;
	}

	if (end - eocd < 21) {
		msg_debug_task ("zip archive is invalid (short EOCD)");

		return;
	}


	memcpy (&cd_size, eocd + 12, sizeof (cd_size));
	cd_size = GUINT32_FROM_LE (cd_size);
	memcpy (&cd_offset, eocd + 16, sizeof (cd_offset));
	cd_offset = GUINT32_FROM_LE (cd_offset);

	/* We need to check sanity as well */
	if (cd_offset + cd_size != (guint)(eocd - start)) {
		msg_debug_task ("zip archive is invalid (bad size/offset for CD)");

		return;
	}

	cd = start + cd_offset;

	arch = rspamd_mempool_alloc0 (task->task_pool, sizeof (*arch));
	arch->files = g_ptr_array_new ();
	arch->type = RSPAMD_ARCHIVE_ZIP;
	rspamd_mempool_add_destructor (task->task_pool, rspamd_archive_dtor,
			arch);

	while (cd < eocd) {
		/* Read central directory record */
		if (eocd - cd < cd_basic_len ||
				memcmp (cd, cd_magic, sizeof (cd_magic)) != 0) {
			msg_debug_task ("zip archive is invalid (bad cd record)");

			return;
		}

		memcpy (&comp_size, cd + 20, sizeof (guint32));
		comp_size = GUINT32_FROM_LE (comp_size);
		memcpy (&uncomp_size, cd + 24, sizeof (guint32));
		uncomp_size = GUINT32_FROM_LE (uncomp_size);
		memcpy (&fname_len, cd + 28, sizeof (fname_len));
		fname_len = GUINT16_FROM_LE (fname_len);
		memcpy (&extra_len, cd + 30, sizeof (extra_len));
		extra_len = GUINT16_FROM_LE (extra_len);
		memcpy (&comment_len, cd + 32, sizeof (comment_len));
		comment_len = GUINT16_FROM_LE (comment_len);

		if (cd + fname_len + comment_len + extra_len + cd_basic_len > eocd) {
			msg_debug_task ("zip archive is invalid (too large cd record)");

			return;
		}

		f = g_slice_alloc0 (sizeof (*f));
		f->fname = g_string_new_len (cd + cd_basic_len, fname_len);
		f->compressed_size = comp_size;
		f->uncompressed_size = uncomp_size;
		g_ptr_array_add (arch->files, f);
		msg_debug_task ("found file in zip archive: %v", f->fname);

		cd += fname_len + comment_len + extra_len + cd_basic_len;
	}

	part->flags |= RSPAMD_MIME_PART_ARCHIVE;
	part->specific.arch = arch;

	if (part->cd) {
		arch->archive_name = &part->cd->filename;
	}

	arch->size = part->parsed_data.len;
}

static inline gint
rspamd_archive_rar_read_vint (const guchar *start, gsize remain, guint64 *res)
{
	/*
	 * From http://www.rarlab.com/technote.htm:
	 * Variable length integer. Can include one or more bytes, where
	 * lower 7 bits of every byte contain integer data and highest bit
	 * in every byte is the continuation flag.
	 * If highest bit is 0, this is the last byte in sequence.
	 * So first byte contains 7 least significant bits of integer and
	 * continuation flag. Second byte, if present, contains next 7 bits and so on.
	 */
	guint64 t = 0;
	guint shift = 0;
	const guchar *p = start;

	while (remain > 0 && shift <= 57) {
		if (*p & 0x80) {
			t |= ((guint64)(*p & 0x7f)) << shift;
		}
		else {
			t |= ((guint64)(*p & 0x7f)) << shift;
			p ++;
			break;
		}

		shift += 7;
		p++;
		remain --;
	}

	if (remain == 0 || shift > 64) {
		return -1;
	}

	*res = GUINT64_FROM_LE (t);

	return p - start;
}

#define RAR_SKIP_BYTES(n) do { \
	if ((n) <= 0) { \
		msg_debug_task ("rar archive is invalid (bad skip value)"); \
		return; \
	} \
	if ((gsize)(end - p) < (n)) { \
		msg_debug_task ("rar archive is invalid (truncated)"); \
		return; \
	} \
	p += (n); \
} while (0)

#define RAR_READ_VINT() do { \
	r = rspamd_archive_rar_read_vint (p, end - p, &vint); \
	if (r == -1) { \
		msg_debug_task ("rar archive is invalid (bad vint)"); \
		return; \
	} \
	else if (r == 0) { \
		msg_debug_task ("rar archive is invalid (BAD vint offset)"); \
		return; \
	}\
} while (0)

#define RAR_READ_VINT_SKIP() do { \
	r = rspamd_archive_rar_read_vint (p, end - p, &vint); \
	if (r == -1) { \
		msg_debug_task ("rar archive is invalid (bad vint)"); \
		return; \
	} \
	p += r; \
} while (0)

#define RAR_READ_UINT16(n) do { \
	if (end - p < (glong)sizeof (guint16)) { \
		msg_debug_task ("rar archive is invalid (bad int16)"); \
		return; \
	} \
	n = p[0] + (p[1] << 8); \
	p += sizeof (guint16); \
} while (0)

#define RAR_READ_UINT32(n) do { \
	if (end - p < (glong)sizeof (guint32)) { \
		msg_debug_task ("rar archive is invalid (bad int32)"); \
		return; \
	} \
	n = (guint)p[0] + ((guint)p[1] << 8) + ((guint)p[2] << 16) + ((guint)p[3] << 24); \
	p += sizeof (guint32); \
} while (0)

static void
rspamd_archive_process_rar_v4 (struct rspamd_task *task, const guchar *start,
		const guchar *end, struct rspamd_mime_part *part)
{
	const guchar *p = start, *start_section;
	guint8 type;
	guint flags;
	guint64 sz, comp_sz = 0, uncomp_sz = 0;
	struct rspamd_archive *arch;
	struct rspamd_archive_file *f;

	arch = rspamd_mempool_alloc0 (task->task_pool, sizeof (*arch));
	arch->files = g_ptr_array_new ();
	arch->type = RSPAMD_ARCHIVE_RAR;
	rspamd_mempool_add_destructor (task->task_pool, rspamd_archive_dtor,
			arch);

	while (p < end) {
		/* Crc16 */
		start_section = p;
		RAR_SKIP_BYTES (sizeof (guint16));
		type = *p;
		p ++;
		RAR_READ_UINT16 (flags);

		if (type == 0x73) {
			/* Main header, check for encryption */
			if (flags & 0x80) {
				arch->flags |= RSPAMD_ARCHIVE_ENCRYPTED;
				goto end;
			}
		}

		RAR_READ_UINT16 (sz);

		if (flags & 0x8000) {
			/* We also need to read ADD_SIZE element */
			guint32 tmp;

			RAR_READ_UINT32 (tmp);
			sz += tmp;
			/* This is also used as PACK_SIZE */
			comp_sz = tmp;
		}

		if (sz == 0) {
			/* Zero sized block - error */
			msg_debug_task ("rar archive is invalid (zero size block)");

			return;
		}

		if (type == 0x74) {
			guint fname_len;

			/* File header */
			/* Uncompressed size */
			RAR_READ_UINT32 (uncomp_sz);
			/* Skip to NAME_SIZE element */
			RAR_SKIP_BYTES (11);
			RAR_READ_UINT16 (fname_len);

			if (fname_len == 0 || fname_len > (gsize)(end - p)) {
				msg_debug_task ("rar archive is invalid (bad fileame size)");

				return;
			}

			/* Attrs */
			RAR_SKIP_BYTES (4);

			if (flags & 0x100) {
				/* We also need to read HIGH_PACK_SIZE */
				guint32 tmp;

				RAR_READ_UINT32 (tmp);
				sz += tmp;
				comp_sz += tmp;
				/* HIGH_UNP_SIZE  */
				RAR_READ_UINT32 (tmp);
				uncomp_sz += tmp;
			}

			f = g_slice_alloc0 (sizeof (*f));

			if (flags & 0x200) {
				/* We have unicode + normal version */
				guchar *tmp;

				tmp = memchr (p, '\0', fname_len);

				if (tmp != NULL) {
					/* Just use ASCII version */
					f->fname = g_string_new_len (p, tmp - p);
				}
				else {
					/* We have UTF8 filename, use it as is */
					f->fname = g_string_new_len (p, fname_len);
				}
			}
			else {
				f->fname = g_string_new_len (p, fname_len);
			}

			f->compressed_size = comp_sz;
			f->uncompressed_size = uncomp_sz;

			if (flags & 0x4) {
				f->flags |= RSPAMD_ARCHIVE_FILE_ENCRYPTED;
			}

			g_ptr_array_add (arch->files, f);
		}

		p = start_section;
		RAR_SKIP_BYTES (sz);
	}

end:
	part->flags |= RSPAMD_MIME_PART_ARCHIVE;
	part->specific.arch = arch;
	arch->archive_name = &part->cd->filename;
	arch->size = part->parsed_data.len;
}

static void
rspamd_archive_process_rar (struct rspamd_task *task,
		struct rspamd_mime_part *part)
{
	const guchar *p, *end, *section_start;
	const guchar rar_v5_magic[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00},
			rar_v4_magic[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
	const guint rar_encrypted_header = 4, rar_main_header = 1,
			rar_file_header = 2;
	guint64 vint, sz, comp_sz = 0, uncomp_sz = 0, flags = 0, type = 0;
	struct rspamd_archive *arch;
	struct rspamd_archive_file *f;
	gint r;

	p = part->parsed_data.begin;
	end = p + part->parsed_data.len;

	if ((gsize)(end - p) <= sizeof (rar_v5_magic)) {
		msg_debug_task ("rar archive is invalid (too small)");

		return;
	}

	if (memcmp (p, rar_v5_magic, sizeof (rar_v5_magic)) == 0) {
		p += sizeof (rar_v5_magic);
	}
	else if (memcmp (p, rar_v4_magic, sizeof (rar_v4_magic)) == 0) {
		p += sizeof (rar_v4_magic);

		rspamd_archive_process_rar_v4 (task, p, end, part);
		return;
	}
	else {
		msg_debug_task ("rar archive is invalid (no rar magic)");

		return;
	}

	/* Rar v5 format */
	arch = rspamd_mempool_alloc0 (task->task_pool, sizeof (*arch));
	arch->files = g_ptr_array_new ();
	arch->type = RSPAMD_ARCHIVE_RAR;
	rspamd_mempool_add_destructor (task->task_pool, rspamd_archive_dtor,
			arch);

	/* Now we can have either encryption header or archive header */
	/* Crc 32 */
	RAR_SKIP_BYTES (sizeof (guint32));
	/* Size */
	RAR_READ_VINT_SKIP ();
	sz = vint;
	/* Type */
	section_start = p;
	RAR_READ_VINT_SKIP ();
	type = vint;
	/* Header flags */
	RAR_READ_VINT_SKIP ();
	flags = vint;

	if (flags & 0x1) {
		/* Have extra zone */
		RAR_READ_VINT_SKIP ();
	}
	if (flags & 0x2) {
		/* Data zone is presented */
		RAR_READ_VINT_SKIP ();
		sz += vint;
	}

	if (type == rar_encrypted_header) {
		/* We can't read any further information as archive is encrypted */
		arch->flags |= RSPAMD_ARCHIVE_ENCRYPTED;
		goto end;
	}
	else if (type != rar_main_header) {
		msg_debug_task ("rar archive is invalid (bad main header)");

		return;
	}

	/* Nothing useful in main header */
	p = section_start;
	RAR_SKIP_BYTES (sz);

	while (p < end) {
		/* Read the next header */
		/* Crc 32 */
		RAR_SKIP_BYTES (sizeof (guint32));
		/* Size */
		RAR_READ_VINT_SKIP ();

		sz = vint;
		if (sz == 0) {
			/* Zero sized block - error */
			msg_debug_task ("rar archive is invalid (zero size block)");

			return;
		}

		section_start = p;
		/* Type */
		RAR_READ_VINT_SKIP ();
		type = vint;
		/* Header flags */
		RAR_READ_VINT_SKIP ();
		flags = vint;

		if (flags & 0x1) {
			/* Have extra zone */
			RAR_READ_VINT_SKIP ();
		}
		if (flags & 0x2) {
			/* Data zone is presented */
			RAR_READ_VINT_SKIP ();
			sz += vint;
			comp_sz = vint;
		}

		if (type != rar_file_header) {
			p = section_start;
			RAR_SKIP_BYTES (sz);
		}
		else {
			/* We have a file header, go forward */
			guint64 fname_len;

			/* File header specific flags */
			RAR_READ_VINT_SKIP ();
			flags = vint;

			/* Unpacked size */
			RAR_READ_VINT_SKIP ();
			uncomp_sz = vint;
			/* Attributes */
			RAR_READ_VINT_SKIP ();

			if (flags & 0x2) {
				/* Unix mtime */
				RAR_SKIP_BYTES (sizeof (guint32));
			}
			if (flags & 0x4) {
				/* Crc32 */
				RAR_SKIP_BYTES (sizeof (guint32));
			}

			/* Compression */
			RAR_READ_VINT_SKIP ();
			/* Host OS */
			RAR_READ_VINT_SKIP ();
			/* Filename length (finally!) */
			RAR_READ_VINT_SKIP ();
			fname_len = vint;

			if (fname_len == 0 || fname_len > (gsize)(end - p)) {
				msg_debug_task ("rar archive is invalid (bad fileame size)");

				return;
			}

			f = g_slice_alloc0 (sizeof (*f));
			f->uncompressed_size = uncomp_sz;
			f->compressed_size = comp_sz;
			f->fname = g_string_new_len (p, fname_len);
			g_ptr_array_add (arch->files, f);

			/* Restore p to the beginning of the header */
			p = section_start;
			RAR_SKIP_BYTES (sz);
		}
	}

end:
part->flags |= RSPAMD_MIME_PART_ARCHIVE;
	part->specific.arch = arch;
	if (part->cd != NULL) {
		arch->archive_name = &part->cd->filename;
	}
	arch->size = part->parsed_data.len;
}

static inline gint
rspamd_archive_7zip_read_vint (const guchar *start, gsize remain, guint64 *res)
{
	/*
	 * REAL_UINT64 means real UINT64.
	 * UINT64 means real UINT64 encoded with the following scheme:
	 *
	 * Size of encoding sequence depends from first byte:
	 * First_Byte  Extra_Bytes        Value
	 * (binary)
	 * 0xxxxxxx               : ( xxxxxxx           )
	 * 10xxxxxx    BYTE y[1]  : (  xxxxxx << (8 * 1)) + y
	 * 110xxxxx    BYTE y[2]  : (   xxxxx << (8 * 2)) + y
	 * ...
	 * 1111110x    BYTE y[6]  : (       x << (8 * 6)) + y
	 * 11111110    BYTE y[7]  :                         y
	 * 11111111    BYTE y[8]  :                         y
	 */
	guchar t;

	if (remain == 0) {
		return -1;
	}

	t = *start;

	if (!isset (&t, 7)) {
		/* Trivial case */
		*res = t;
		return 1;
	}
	else if (t == 0xFF) {
		if (remain >= sizeof (guint64) + 1) {
			memcpy (res, start + 1, sizeof (guint64));
			*res = GUINT64_FROM_LE (*res);

			return sizeof (guint64) + 1;
		}
	}
	else {
		gint cur_bit = 6, intlen = 1;
		const guchar bmask = 0xFF;
		guint64 tgt;

		while (cur_bit > 0) {
			if (!isset (&t, cur_bit)) {
				if (remain >= intlen + 1) {
					memcpy (&tgt, start + 1, intlen);
					tgt = GUINT64_FROM_LE (tgt);
					/* Shift back */
					tgt >>= sizeof (tgt) - NBBY * intlen;
					/* Add masked value */
					tgt += (guint64)(t & (bmask >> (NBBY - cur_bit)))
							<< (NBBY * intlen);
					*res = tgt;

					return intlen + 1;
				}
			}
			cur_bit --;
			intlen ++;
		}
	}

	return -1;
}

#define SZ_READ_VINT_SKIP() do { \
	r = rspamd_archive_7zip_read_vint (p, end - p, &vint); \
	if (r == -1) { \
		msg_debug_task ("7z archive is invalid (bad vint)"); \
		return; \
	} \
	p += r; \
} while (0)
#define SZ_READ_VINT(var) do { \
	r = rspamd_archive_7zip_read_vint (p, end - p, &(var)); \
	if (r == -1) { \
		msg_debug_task ("7z archive is invalid (bad vint)"); \
		return; \
	} \
	p += r; \
} while (0)

static void
rspamd_archive_process_7zip (struct rspamd_task *task,
		struct rspamd_mime_part *part)
{
	struct rspamd_archive *arch;
	const guchar *p, *end;
	const guchar sz_magic[] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
	guint64 section_offset = 0, section_length = 0;
	gint r;

	p = part->parsed_data.begin;
	end = p + part->parsed_data.len;

	if (end - p <= sizeof (guint64) + sizeof (guint32) ||
			memcmp (p, sz_magic, sizeof (sz_magic)) != 0) {
		msg_debug_task ("7z archive is invalid (no 7z magic)");

		return;
	}

	arch = rspamd_mempool_alloc0 (task->task_pool, sizeof (*arch));
	arch->files = g_ptr_array_new ();
	arch->type = RSPAMD_ARCHIVE_7ZIP;
	rspamd_mempool_add_destructor (task->task_pool, rspamd_archive_dtor,
			arch);

	/* Magic (6 bytes) + version (2 bytes) + crc32 (4 bytes) */
	p += sizeof (guint64) + sizeof (guint32);

	SZ_READ_VINT(section_offset);
	SZ_READ_VINT(section_length);

	part->flags |= RSPAMD_MIME_PART_ARCHIVE;
	part->specific.arch = arch;
	if (part->cd != NULL) {
		arch->archive_name = &part->cd->filename;
	}
	arch->size = part->parsed_data.len;
}

static gboolean
rspamd_archive_cheat_detect (struct rspamd_mime_part *part, const gchar *str,
		const guchar *magic_start, gsize magic_len)
{
	struct rspamd_content_type *ct;
	const gchar *p;
	rspamd_ftok_t srch, *fname;

	ct = part->ct;
	RSPAMD_FTOK_ASSIGN (&srch, "application");

	if (ct && ct->type.len && ct->subtype.len > 0 && rspamd_ftok_cmp (&ct->type,
			&srch) == 0) {
		if (rspamd_substring_search_caseless (ct->subtype.begin, ct->subtype.len,
				str, strlen (str)) != -1) {
			/* We still need to check magic, see #1848 */
			if (magic_start != NULL) {
				if (part->parsed_data.len > magic_len &&
						memcmp (part->parsed_data.begin,
								magic_start, magic_len) == 0) {
					return TRUE;
				}
				/* No magic, refuse this type of archive */
				return FALSE;
			}
			else {
				return TRUE;
			}
		}
	}

	if (part->cd) {
		fname = &part->cd->filename;

		if (fname && fname->len > strlen (str)) {
			p = fname->begin + fname->len - strlen (str);

			if (rspamd_lc_cmp (p, str, strlen (str)) == 0) {
				if (*(p - 1) == '.') {
					if (magic_start != NULL) {
						if (part->parsed_data.len > magic_len &&
								memcmp (part->parsed_data.begin,
										magic_start, magic_len) == 0) {
							return TRUE;
						}
						/* No magic, refuse this type of archive */
						return FALSE;
					}

					return TRUE;
				}
			}
		}

		if (magic_start != NULL) {
			if (part->parsed_data.len > magic_len && memcmp (part->parsed_data.begin,
					magic_start, magic_len) == 0) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

void
rspamd_archives_process (struct rspamd_task *task)
{
	guint i;
	struct rspamd_mime_part *part;
	const guchar rar_magic[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07};
	const guchar zip_magic[] = {0x50, 0x4b, 0x03, 0x04};
	const guchar sz_magic[] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};

	for (i = 0; i < task->parts->len; i ++) {
		part = g_ptr_array_index (task->parts, i);

		if (part->parsed_data.len > 0) {
			if (rspamd_archive_cheat_detect (part, "zip",
					zip_magic, sizeof (zip_magic))) {
				rspamd_archive_process_zip (task, part);
			}
			else if (rspamd_archive_cheat_detect (part, "rar",
					rar_magic, sizeof (rar_magic))) {
				rspamd_archive_process_rar (task, part);
			}
			else if (rspamd_archive_cheat_detect (part, "7z",
					sz_magic, sizeof (sz_magic))) {
				rspamd_archive_process_7zip (task, part);
			}

		}
	}
}


const gchar *
rspamd_archive_type_str (enum rspamd_archive_type type)
{
	const gchar *ret = "unknown";

	switch (type) {
	case RSPAMD_ARCHIVE_ZIP:
		ret = "zip";
		break;
	case RSPAMD_ARCHIVE_RAR:
		ret = "rar";
		break;
	case RSPAMD_ARCHIVE_7ZIP:
		ret = "7z";
		break;
	}

	return ret;
}

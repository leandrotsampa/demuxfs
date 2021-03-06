/* 
 * Copyright (c) 2008-2018, Lucas C. Villa Real <lucasvr@gobolinux.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. Neither the name of GoboLinux nor the names of its contributors may
 * be used to endorse or promote products derived from this software
 * without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "demuxfs.h"
#include "fsutils.h"
#include "hash.h"
#include "ts.h"
#include "byteops.h"
#include "tables/psi.h"
#include "tables/pat.h"
#include "tables/pmt.h"
#include "tables/nit.h"

void pat_free(struct pat_table *pat)
{
	if (pat->dentry && pat->dentry->name)
		fsutils_dispose_tree(pat->dentry);
	else if (pat->dentry)
		/* Dentry has simply been calloc'ed */
		free(pat->dentry);

	/* Free the pat table structure */
	if (pat->programs)
		free(pat->programs);
	free(pat);
}

bool pat_announces_service(uint16_t service_id, struct demuxfs_data *priv)
{
	struct dentry *pat_programs;
	char buf[PATH_MAX];

	snprintf(buf, sizeof(buf), "/%s/%s/%s", FS_PAT_NAME, FS_CURRENT_NAME, FS_PROGRAMS_NAME);
	pat_programs = fsutils_get_dentry(priv->root, buf);
	if (! pat_programs) {
		TS_WARNING("%s doesn't exit", buf);
		return false;
	}

	snprintf(buf, sizeof(buf), "%#04x", service_id);
	return fsutils_get_child(pat_programs, buf) ? true : false;
}

/* PAT private stuff */
static void pat_populate(struct pat_table *pat, struct dentry *parent, 
		struct demuxfs_data *priv)
{
	/* "Programs" directory */
	struct dentry *dentry = CREATE_DIRECTORY(parent, FS_PROGRAMS_NAME);

	/* Append new parsers to the list of known PIDs */
	for (uint16_t i=0; i<pat->num_programs; ++i) {
		char name[32], target[PATH_MAX];
		uint16_t pid = pat->programs[i].pid;
		uint16_t program_number = pat->programs[i].program_number;

		/* 
		 * XXX: we may run into a problem as we just make use the PID, whereas 
		 * in nit.c and pmt.c both the PID and the table_id are used as key.
		 */
		void *existing_parser = hashtable_get(priv->psi_tables, pid);

		/* Create a symlink which points to this dentry in the PMT */
		snprintf(name, sizeof(name), "%#04x", pat->programs[i].program_number);
		if (program_number == 0) {
			snprintf(target, sizeof(target), "../../../%s/%s",
				FS_NIT_NAME, FS_CURRENT_NAME);
			if (! existing_parser)
				hashtable_add(priv->psi_parsers, pid, nit_parse, NULL);
		} else {
			snprintf(target, sizeof(target), "../../../%s/%#04x/%s",
				FS_PMT_NAME, pid, FS_CURRENT_NAME);
			if (! existing_parser)
				hashtable_add(priv->psi_parsers, pid, pmt_parse, NULL);
		}
		CREATE_SYMLINK(dentry, name, target);
	}
}

static void pat_create_directory(struct pat_table *pat, struct demuxfs_data *priv)
{
	struct dentry *version_dentry;

	/* Create a directory named "PAT" and populate it with files */
	pat->dentry->name = strdup(FS_PAT_NAME);
	pat->dentry->mode = S_IFDIR | 0555;
	CREATE_COMMON(priv->root, pat->dentry);

	/* Create the versioned dir and update the Current symlink */
	version_dentry = fsutils_create_version_dir(pat->dentry, pat->version_number);

	psi_populate((void **) &pat, version_dentry);
	pat_populate(pat, version_dentry, priv);
}

int pat_parse(const struct ts_header *header, const char *payload, uint32_t payload_len, 
		struct demuxfs_data *priv)
{
	struct pat_table *current_pat = NULL;
	struct pat_table *pat = (struct pat_table *) calloc(1, sizeof(struct pat_table));
	assert(pat);

	pat->dentry = (struct dentry *) calloc(1, sizeof(struct dentry));
	assert(pat->dentry);

	/* Copy data up to the first loop entry */
	int ret = psi_parse((struct psi_common_header *) pat, payload, payload_len);
	if (ret < 0) {
		free(pat->dentry);
		free(pat);
		return 0;
	}

	/* Set hash key and check if there's already one version of this table in the hash */
	pat->dentry->inode = TS_PACKET_HASH_KEY(header, pat);
	current_pat = hashtable_get(priv->psi_tables, pat->dentry->inode);

	/* Check whether we should keep processing this packet or not */
	if (! pat->current_next_indicator || (current_pat && current_pat->version_number == pat->version_number)) {
		free(pat->dentry);
		free(pat);
		return 0;
	}
	TS_INFO("PAT parser: pid=%#x, table_id=%#x, current_pat=%p, pat->version_number=%#x, len=%d", 
			header->pid, pat->table_id, current_pat, pat->version_number, payload_len);

	/* Parse PAT specific bits */
	pat->num_programs = (pat->section_length - 
		/* transport_stream_id */ 2 -
		/* reserved/version_number/current_next_indicator */ 1 -
		/* section_number */ 1 -
		/* last_section_number */ 1 -
		/* crc32 */ 4) / 4;

	pat->programs = (struct pat_program *) calloc(pat->num_programs, sizeof(struct pat_program));
	assert(pat->programs);

	for (uint16_t i=0; i<pat->num_programs; ++i) {
		uint16_t offset = 8 + (i * 4);
		pat->programs[i].program_number = CONVERT_TO_16(payload[offset], payload[offset+1]);
		pat->programs[i].reserved = payload[offset+2] >> 4;
		pat->programs[i].pid = CONVERT_TO_16(payload[offset+2], payload[offset+3]) & 0x1fff;
	}

	pat_create_directory(pat, priv);

	if (current_pat) {
		fsutils_migrate_children(current_pat->dentry, pat->dentry);
		fsutils_dispose_tree(current_pat->dentry);
		hashtable_del(priv->psi_tables, current_pat->dentry->inode);
	}
	hashtable_add(priv->psi_tables, pat->dentry->inode, pat, (hashtable_free_function_t) pat_free);

	return 0;
}

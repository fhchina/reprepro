/*  This file is part of "reprepro"
 *  Copyright (C) 2006,2007,2008 Bernhard R. Link
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02111-1301  USA
 */
#include <config.h>

#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include "error.h"
#include "ignore.h"
#include "mprintf.h"
#include "filecntl.h"
#include "strlist.h"
#include "dirs.h"
#include "names.h"
#include "checksums.h"
#include "chunks.h"
#include "target.h"
#include "signature.h"
#include "binaries.h"
#include "sources.h"
#include "dpkgversions.h"
#include "uploaderslist.h"
#include "guesscomponent.h"
#include "log.h"
#include "override.h"
#include "tracking.h"
#include "incoming.h"
#include "files.h"
#include "configparser.h"
#include "changes.h"

extern int verbose;

enum permitflags {
	/* do not error out on unused files */
	pmf_unused_files = 0,
	/* allow .changes file to specify multipe distributions */
	pmf_multiple_distributions,
	/* do not error out if there already is a newer package */
	pmf_oldpackagenewer,
	pmf_COUNT /* must be last */
};
enum cleanupflags {
	/* delete everything referenced by a .changes file
	 * when it is not accepted */
	cuf_on_deny = 0,
	/* check owner when deleting on_deny */
	cuf_on_deny_check_owner,
	/* delete everything referenced by a .changes on errors
	 * after accepting that .changes file*/
	cuf_on_error,
	/* delete unused files after sucessfully
	 * processing the used ones */
	cuf_unused_files,
	cuf_COUNT /* must be last */
};

struct incoming {
	/* by incoming_parse: */
	char *directory;
	char *tempdir;
	struct strlist allow;
	struct distribution **allow_into;
	struct distribution *default_into;
	/* by incoming_prepare: */
	struct strlist files;
	bool *processed;
	bool *delete;
	bool permit[pmf_COUNT];
	bool cleanup[cuf_COUNT];
	/* only to ease parsing: */
	char *name; size_t lineno;
};
#define BASENAME(i,ofs) (i)->files.values[ofs]
/* the changes file is always the first one listed */
#define changesfile(c) (c->files)

static void incoming_free(/*@only@*/ struct incoming *i) {
	if( i == NULL )
		return;
	free(i->name);
	free(i->tempdir);
	free(i->directory);
	strlist_done(&i->allow);
	free(i->allow_into);
	strlist_done(&i->files);
	free(i->processed);
	free(i->delete);
	free(i);
}

static retvalue incoming_prepare(struct incoming *i) {
	DIR *dir;
	struct dirent *ent;
	retvalue r;
	int ret;

	/* TODO: decide whether to clean this directory first ... */
	r = dirs_make_recursive(i->tempdir);
	if( RET_WAS_ERROR(r) )
		return r;
	dir = opendir(i->directory);
	if( dir == NULL ) {
		int e = errno;
		fprintf(stderr, "Cannot scan '%s': %s\n", i->directory, strerror(e));
		return RET_ERRNO(e);
	}
	while( (ent = readdir(dir)) != NULL ) {
		if( ent->d_name[0] == '.' )
			continue;
		/* this should be impossible to hit.
		 * but given utf-8 encoding filesystems and
		 * overlong slashes, better check than be sorry */
		if( strchr(ent->d_name, '/') != NULL )
			continue;
		r = strlist_add_dup(&i->files, ent->d_name) ;
		if( RET_WAS_ERROR(r) ) {
			(void)closedir(dir);
			return r;
		}
	}
	ret = closedir(dir);
	if( ret != 0 ) {
		int e = errno;
		fprintf(stderr, "Error scanning '%s': %s\n", i->directory, strerror(e));
		return RET_ERRNO(e);
	}
	i->processed = calloc(i->files.count, sizeof(bool));
	if( i->processed == NULL )
		return RET_ERROR_OOM;
	i->delete = calloc(i->files.count, sizeof(bool));
	if( i->delete == NULL )
		return RET_ERROR_OOM;
	return RET_OK;
}

struct read_incoming_data {
	/*@temp@*/const char *name;
	/*@temp@*/struct distribution *distributions;
	struct incoming *i;
	/*@temp@*/const char *basedir;
};

static retvalue translate(struct distribution *distributions, struct strlist *names, struct distribution ***r) {
	struct distribution **d;
	int j;

	d = calloc(names->count,sizeof(struct distribution*));
	if( d == NULL )
		return RET_ERROR_OOM;
	for( j = 0 ; j < names->count ; j++ ) {
		d[j] = distribution_find(distributions, names->values[j]);
		if( d[j] == NULL ) {
			free(d);
			return RET_ERROR;
		}
	}
	*r = d;
	return RET_OK;
}

CFstartparse(incoming) {
	CFstartparseVAR(incoming,result_p);
	struct incoming *i;

	i = calloc(1,sizeof(struct incoming));
	if( i == NULL )
		return RET_ERROR_OOM;
	*result_p = i;
	return RET_OK;
}

CFfinishparse(incoming) {
	CFfinishparseVARS(incoming,i,last,d);

	if( !complete || strcmp(i->name, d->name) != 0) {
		incoming_free(i);
		return RET_NOTHING;
	}
	if( d->i != NULL ) {
		fprintf(stderr, "Multiple definitions of '%s' within '%s': first started at line %u, second at line %u!\n",
				d->name,
				config_filename(iter),
				(unsigned int)d->i->lineno,
				config_firstline(iter));
		incoming_free(i);
		incoming_free(d->i);
		d->i = NULL;
		return RET_ERROR;
	}
	if( i->tempdir[0] != '/' ) {
		char *n = calc_dirconcat(d->basedir, i->tempdir);
		if( n == NULL ) {
			incoming_free(i);
			return RET_ERROR_OOM;
		}
		free(i->tempdir);
		i->tempdir = n;
	}
	if( i->directory[0] != '/' ) {
		char *n = calc_dirconcat(d->basedir, i->directory);
		if( n == NULL ) {
			incoming_free(i);
			return RET_ERROR_OOM;
		}
		free(i->directory);
		i->directory = n;
	}
	if( i->default_into == NULL && i->allow.count == 0 ) {
		fprintf(stderr,
"There is neither an 'Allow' nor a 'Default' definition in rule '%s'\n"
"(starting at line %u, ending at line %u of %s)!\n"
"Aborting as nothing would be let in.\n",
				d->name,
				config_firstline(iter), config_line(iter),
				config_filename(iter));
			incoming_free(i);
			return RET_ERROR;
	}

	d->i = i;
	i->lineno = config_firstline(iter);
	/* only suppreses the last unused warning: */
	*last = i;
	return RET_OK;
}

CFSETPROC(incoming,default) {
	CFSETPROCVARS(incoming,i,d);
	char *default_into;
	retvalue r;

	r = config_getonlyword(iter, headername, NULL, &default_into);
	assert( r != RET_NOTHING );
	if( RET_WAS_ERROR(r) )
		return r;
	i->default_into = distribution_find(d->distributions, default_into);
	free(default_into);
	return ( i->default_into == NULL )?RET_ERROR:RET_OK;
}

CFSETPROC(incoming,allow) {
	CFSETPROCVARS(incoming,i,d);
	struct strlist allow_into;
	retvalue r;

	r = config_getsplitwords(iter, headername, &i->allow, &allow_into);
	assert( r != RET_NOTHING );
	if( RET_WAS_ERROR(r) )
		return r;
	assert( i->allow.count == allow_into.count );
	r = translate(d->distributions, &allow_into, &i->allow_into);
	strlist_done(&allow_into);
	if( RET_WAS_ERROR(r) )
		return r;
	return RET_OK;
}

CFSETPROC(incoming,permit) {
	CFSETPROCVARS(incoming,i,d);
	static const struct constant const permitconstants[] = {
		{ "unused_files",	pmf_unused_files},
		{ "older_version",	pmf_oldpackagenewer},
		/* not yet implemented:
		   { "downgrade",		pmf_downgrade},
		 */
		{ NULL, -1}
	};

	if( IGNORABLE(unknownfield) )
		return config_getflags(iter, headername, permitconstants,
				i->permit, true, "");
	else if( i->name == NULL )
		return config_getflags(iter, headername, permitconstants,
				i->permit, false,
"\n(try put Name: before Permit: to ignore if it is from the wrong rule");
	else if( strcmp(i->name, d->name) != 0 )
		return config_getflags(iter, headername, permitconstants,
				i->permit, true,
" (but not within the rule we are intrested in.)");
	else
		return config_getflags(iter, headername, permitconstants,
				i->permit, false,
" (use --ignore=unknownfield to ignore this)\n");

}

CFSETPROC(incoming,cleanup) {
	CFSETPROCVARS(incoming,i,d);
	static const struct constant const cleanupconstants[] = {
		{ "unused_files", cuf_unused_files},
		{ "on_deny", cuf_on_deny},
		/* not yet implemented
		{ "on_deny_check_owner", cuf_on_deny_check_owner},
		 */
		{ "on_error", cuf_on_error},
		{ NULL, -1}
	};

	if( IGNORABLE(unknownfield) )
		return config_getflags(iter, headername, cleanupconstants,
				i->cleanup, true, "");
	else if( i->name == NULL )
		return config_getflags(iter, headername, cleanupconstants,
				i->cleanup, false,
"\n(try put Name: before Cleanup: to ignore if it is from the wrong rule");
	else if( strcmp(i->name, d->name) != 0 )
		return config_getflags(iter, headername, cleanupconstants,
				i->cleanup, true,
" (but not within the rule we are intrested in.)");
	else
		return config_getflags(iter, headername, cleanupconstants,
				i->cleanup, false,
" (use --ignore=unknownfield to ignore this)\n");
}

CFvalueSETPROC(incoming, name)
CFdirSETPROC(incoming, tempdir)
CFdirSETPROC(incoming, directory)
CFtruthSETPROC2(incoming, multiple, permit[pmf_multiple_distributions])

static const struct configfield incomingconfigfields[] = {
	CFr("Name", incoming, name),
	CFr("TempDir", incoming, tempdir),
	CFr("IncomingDir", incoming, directory),
	CF("Default", incoming, default),
	CF("Allow", incoming, allow),
	CF("Multiple", incoming, multiple),
	CF("Cleanup", incoming, cleanup),
	CF("Permit", incoming, permit)
};

static retvalue incoming_init(const char *basedir,const char *confdir, struct distribution *distributions, const char *name, /*@out@*/struct incoming **result) {
	retvalue r;
	struct read_incoming_data imports;

	imports.name = name;
	imports.distributions = distributions;
	imports.i = NULL;
	imports.basedir = basedir;

	r = configfile_parse(confdir, "incoming", IGNORABLE(unknownfield),
			startparseincoming, finishparseincoming,
			incomingconfigfields, ARRAYCOUNT(incomingconfigfields),
			&imports);
	if( RET_WAS_ERROR(r) )
		return r;
	if( imports.i == NULL ) {
		fprintf(stderr, "No definition for '%s' found in '%s/incoming'!\n",
				name, confdir);
		return RET_ERROR_MISSING;
	}

	r = incoming_prepare(imports.i);
	if( RET_WAS_ERROR(r) ) {
		incoming_free(imports.i);
		return r;
	}
	*result = imports.i;
	return r;
}

struct candidate {
	/* from candidate_read */
	int ofs;
	char *control;
	struct strlist keys, allkeys;
	/* from candidate_parse */
	char *source, *sourceversion, *changesversion;
	struct strlist distributions,
		       architectures,
		       binaries;
	bool isbinNMU;
	struct candidate_file {
		/* set by _addfileline */
		struct candidate_file *next;
		int ofs; /* to basename in struct incoming->files */
		filetype type;
		/* all NULL if it is the .changes itself,
		 * otherwise the data from the .changes for this file: */
		char *section;
		char *priority;
		char *architecture;
		char *name;
		/* like above, but updated once files are copied */
		struct checksums *checksums;
		/* set later */
		bool used;
		char *tempfilename;
		/* distribution-unspecific contents of the packages */
		/* - only for FE_BINARY types: */
		struct deb_headers deb;
		/* - only for fe_DSC types */
		struct dsc_headers dsc;
		/* only valid while parsing */
		struct hashes h;
	} *files;
	struct candidate_perdistribution {
		struct candidate_perdistribution *next;
		struct distribution *into;
		bool skip;
		struct candidate_package {
			/* a package is something installing files, including
			 * the pseudo-package for the .changes file, if that is
			 * to be included */
			struct candidate_package *next;
			const struct candidate_file *master;
			char *component;
			struct strlist filekeys;
			/* a list of pointers to the files belonging to those
			 * filekeys, NULL if it does not need linking/copying */
			const struct candidate_file **files;
			/* only for FE_PACKAGE: */
			char *control;
			/* only for fe_DSC */
			char *directory;
			/* true if skipped because already there or newer */
			bool skip;
		} *packages;
	} *perdistribution;
};

static void candidate_file_free(/*@only@*/struct candidate_file *f) {
	checksums_free(f->checksums);
	free(f->section);
	free(f->priority);
	free(f->architecture);
	free(f->name);
	if( FE_BINARY(f->type) )
		binaries_debdone(&f->deb);
	if( f->type == fe_DSC )
		sources_done(&f->dsc);
	if( f->tempfilename != NULL ) {
		(void)unlink(f->tempfilename);
		free(f->tempfilename);
		f->tempfilename = NULL;
	}
	free(f);
}

static void candidate_package_free(/*@only@*/struct candidate_package *p) {
	free(p->control);
	free(p->component);
	free(p->directory);
	strlist_done(&p->filekeys);
	free(p->files);
	free(p);
}

static void candidate_free(/*@only@*/struct candidate *c) {
	if( c == NULL )
		return;
	free(c->control);
	strlist_done(&c->keys);
	strlist_done(&c->allkeys);
	free(c->source);
	free(c->sourceversion);
	free(c->changesversion);
	strlist_done(&c->distributions);
	strlist_done(&c->architectures);
	strlist_done(&c->binaries);
	while( c->perdistribution != NULL ) {
		struct candidate_perdistribution *d = c->perdistribution;
		c->perdistribution = d->next;

		while( d->packages != NULL ) {
			struct candidate_package *p = d->packages;
			d->packages = p->next;
			candidate_package_free(p);
		}
		free(d);
	}
	while( c->files != NULL ) {
		struct candidate_file *f = c->files;
		c->files = f->next;
		candidate_file_free(f);
	}
	free(c);
}

static retvalue candidate_newdistribution(struct candidate *c, struct distribution *distribution) {
	struct candidate_perdistribution *n,**pp = &c->perdistribution;

	while( *pp != NULL ) {
		if( (*pp)->into == distribution )
			return RET_NOTHING;
		pp = &(*pp)->next;
	}
	n = calloc(1, sizeof(struct candidate_perdistribution));
	if( n == NULL )
		return RET_ERROR_OOM;
	n->into = distribution;
	*pp = n;
	return RET_OK;
}

static struct candidate_package *candidate_newpackage(struct candidate_perdistribution *fordistribution, const struct candidate_file *master) {
	struct candidate_package *n,**pp = &fordistribution->packages;

	while( *pp != NULL )
		pp = &(*pp)->next;
	n = calloc(1, sizeof(struct candidate_package));
	if( n != NULL ) {
		n->master = master;
		*pp = n;
	}
	return n;
}

static retvalue candidate_usefile(const struct incoming *i,const struct candidate *c,struct candidate_file *file);

static retvalue candidate_read(struct incoming *i, int ofs, struct candidate **result, bool *broken) {
	struct candidate *n;
	retvalue r;

	n = calloc(1,sizeof(struct candidate));
	if( n == NULL )
		return RET_ERROR_OOM;
	n->ofs = ofs;
	/* first file of any .changes file is the file itself */
	n->files = calloc(1,sizeof(struct candidate_file));
	if( n->files == NULL ) {
		free(n);
		return RET_ERROR_OOM;
	}
	n->files->ofs = n->ofs;
	n->files->type = fe_UNKNOWN;
	r = candidate_usefile(i, n, n->files);
	assert( r != RET_NOTHING );
	if( RET_WAS_ERROR(r) ) {
		candidate_free(n);
		return r;
	}
	assert( n->files->tempfilename != NULL );
	r = signature_readsignedchunk(n->files->tempfilename, BASENAME(i,ofs), &n->control, &n->keys, &n->allkeys, broken);
	assert( r != RET_NOTHING );
	if( RET_WAS_ERROR(r) ) {
		candidate_free(n);
		return r;
	}
	*result = n;
	return RET_OK;
}

static retvalue candidate_addfileline(struct incoming *i, struct candidate *c, const char *fileline) {
	struct candidate_file **p, *n;
	char *basename;
	retvalue r;

	n = calloc(1,sizeof(struct candidate_file));
	if( n == NULL )
		return RET_ERROR_OOM;

	r = changes_parsefileline(fileline, &n->type, &basename,
			&n->h.hashes[cs_md5sum], &n->h.hashes[cs_length],
			&n->section, &n->priority, &n->architecture, &n->name);
	if( RET_WAS_ERROR(r) ) {
		free(n);
		return r;
	}
	n->ofs = strlist_ofs(&i->files, basename);
	if( n->ofs < 0 ) {
		fprintf(stderr,"In '%s': file '%s' not found in the incoming dir!\n", i->files.values[c->ofs], basename);
		free(basename);
		candidate_file_free(n);
		return RET_ERROR_MISSING;
	}
	free(basename);

	p = &c->files;
	while( *p != NULL )
		p = &(*p)->next;
	*p = n;
	return RET_OK;
}

static retvalue candidate_addhashes(struct incoming *i, struct candidate *c, enum checksumtype cs, const struct strlist *lines) {
	int j;

	for( j = 0 ; j < lines->count ; j++ ) {
		const char *fileline = lines->values[j];
		struct candidate_file *f;
		const char *basename;
		struct hash_data hash, size;
		retvalue r;

		r = hashline_parse(BASENAME(i, c->ofs), fileline, cs,
				&basename, &hash, &size);
		if( !RET_IS_OK(r) )
			return r;
		f = c->files;
		while( f != NULL && strcmp(BASENAME(i, f->ofs), basename) != 0 )
			f = f->next;
		if( f == NULL ) {
			fprintf(stderr,
"Warning: Ignoring file '%s' listed in '%s' but not in '%s' of '%s'!\n",
					basename, changes_checksum_names[cs],
					changes_checksum_names[cs_md5sum],
					BASENAME(i, c->ofs));
			continue;
		}
		if( f->h.hashes[cs_length].len != size.len ||
				memcmp(f->h.hashes[cs_length].start,
					size.start, size.len) != 0 ) {
			fprintf(stderr,
"Error: Different size of '%s' listed in '%s' and '%s' of '%s'!\n",
					basename, changes_checksum_names[cs],
					changes_checksum_names[cs_md5sum],
					BASENAME(i, c->ofs));
			return RET_ERROR;
		}
		f->h.hashes[cs] = hash;
	}
	return RET_OK;
}

static retvalue candidate_finalizechecksums(struct incoming *i, struct candidate *c) {
	struct candidate_file *f;
	retvalue r;

	/* store collected hashes as checksums structs,
	 * starting after .changes file: */
	for( f = c->files->next ; f != NULL ; f = f->next ) {
		r = checksums_initialize(&f->checksums, f->h.hashes);
		if( RET_WAS_ERROR(r) )
			return r;
	}
	return RET_OK;
}

static retvalue candidate_parse(struct incoming *i, struct candidate *c) {
	retvalue r;
	struct strlist filelines[cs_hashCOUNT];
	enum checksumtype cs;
	int j;
#define R if( RET_WAS_ERROR(r) ) return r;
#define E(err,...) { \
		if( r == RET_NOTHING ) { \
			fprintf(stderr, "In '%s': " err "\n",BASENAME(i,c->ofs), ## __VA_ARGS__ ); \
			r = RET_ERROR; \
	  	} \
		if( RET_WAS_ERROR(r) ) return r; \
	}
	r = chunk_getnameandversion(c->control, "Source", &c->source, &c->sourceversion);
	E("Missing 'Source' field!");
	r = propersourcename(c->source);
	E("Malforce Source name!");
	if( c->sourceversion != NULL ) {
		r = properversion(c->sourceversion);
		E("Malforce Source Version number!");
	}
	r = chunk_getwordlist(c->control,"Binary",&c->binaries);
	E("Missing 'Binary' field!");
	r = chunk_getwordlist(c->control,"Architecture",&c->architectures);
	E("Missing 'Architecture' field!");
	r = chunk_getvalue(c->control,"Version",&c->changesversion);
	E("Missing 'Version' field!");
	r = properversion(c->changesversion);
	E("Malforce Version number!");
	// TODO: logic to detect binNMUs to warn against sources?
	if( c->sourceversion == NULL ) {
		c->sourceversion = strdup(c->changesversion);
		if( c->sourceversion == NULL )
			return RET_ERROR_OOM;
		c->isbinNMU = false;
	} else {
		int cmp;

		r = dpkgversions_cmp(c->sourceversion, c->changesversion, &cmp);
		R;
		c->isbinNMU = cmp != 0;
	}
	r = chunk_getwordlist(c->control,"Distribution",&c->distributions);
	E("Missing 'Distribution' field!");
	r = chunk_getextralinelist(c->control,
			changes_checksum_names[cs_md5sum],
			&filelines[cs_md5sum]);
	E("Missing '%s' field!", changes_checksum_names[cs_md5sum]);
	for( j = 0 ; j < filelines[cs_md5sum].count ; j++ ) {
		r = candidate_addfileline(i, c, filelines[cs_md5sum].values[j]);
		if( RET_WAS_ERROR(r) ) {
			strlist_done(&filelines[cs_md5sum]);
			return r;
		}
	}
	for( cs = cs_firstEXTENDED ; cs < cs_hashCOUNT ; cs++ ) {
		r = chunk_getextralinelist(c->control,
				changes_checksum_names[cs], &filelines[cs]);

		if( RET_IS_OK(r) )
			r = candidate_addhashes(i, c, cs, &filelines[cs]);
		else
			strlist_init(&filelines[cs]);

		if( RET_WAS_ERROR(r) ) {
			while( cs-- > cs_md5sum )
				strlist_done(&filelines[cs]);
			return r;
		}
	}
	r = candidate_finalizechecksums(i, c);
	for( cs = cs_md5sum ; cs < cs_hashCOUNT ; cs++ )
		strlist_done(&filelines[cs]);
	R;
	if( c->files == NULL || c->files->next == NULL ) {
		fprintf(stderr,"In '%s': Empty 'Files' section!\n",
				BASENAME(i,c->ofs));
		return RET_ERROR;
	}
	return RET_OK;
}

static retvalue candidate_earlychecks(struct incoming *i, struct candidate *c) {
	struct candidate_file *file;
	retvalue r;

	// TODO: allow being more permissive,
	// that will need some more checks later, though
	r = propersourcename(c->source);
	if( RET_WAS_ERROR(r) )
		return r;
	r = properversion(c->sourceversion);
	if( RET_WAS_ERROR(r) )
		return r;
	for( file = c->files ; file != NULL ; file = file->next ) {
		if( !FE_PACKAGE(file->type) )
			continue;
		if( strlist_in(&c->architectures, file->architecture) )
			continue;
		fprintf(stderr, "'%s' is not listed in the Architecture header of '%s' but file '%s' looks like it!\n",
				file->architecture, BASENAME(i,c->ofs),
				BASENAME(i,file->ofs));
		return RET_ERROR;
	}
	return RET_OK;
}

/* Is used before any other candidate fields are set */
static retvalue candidate_usefile(const struct incoming *i,const struct candidate *c,struct candidate_file *file) {
	const char *basename;
	char *origfile,*tempfilename;
	struct checksums *readchecksums;
	retvalue r;
	bool improves;
	const char *p;

	if( file->used && file->tempfilename != NULL )
		return RET_OK;
	assert(file->tempfilename == NULL);
	basename = BASENAME(i,file->ofs);
	for( p = basename; *p != '\0' ; p++ ) {
		if( (0x80 & *(const unsigned char *)p) != 0 ) {
			fprintf(stderr, "Invalid filename '%s' listed in '%s': contains 8-bit characters\n", basename, BASENAME(i,c->ofs));
			return RET_ERROR;
		}
	}
	tempfilename = calc_dirconcat(i->tempdir, basename);
	if( tempfilename == NULL )
		return RET_ERROR_OOM;
	origfile = calc_dirconcat(i->directory, basename);
	if( origfile == NULL ) {
		free(tempfilename);
		return RET_ERROR_OOM;
	}
	(void)unlink(tempfilename);
	r = checksums_copyfile(tempfilename, origfile, &readchecksums);
	free(origfile);
	if( RET_WAS_ERROR(r) ) {
		free(tempfilename);
		return r;
	}
	if( file->checksums == NULL ) {
		file->checksums = readchecksums;
		file->tempfilename = tempfilename;
		file->used = true;
		return RET_OK;
	}
	if( !checksums_check(file->checksums, readchecksums, &improves) ) {
		fprintf(stderr, "ERROR: File '%s' does not match expectations:\n", basename);
		checksums_printdifferences(stderr, file->checksums, readchecksums);
		checksums_free(readchecksums);
		deletefile(tempfilename);
		free(tempfilename);
		return RET_ERROR_WRONG_MD5;
	}
	if( improves ) {
		r = checksums_combine(&file->checksums, readchecksums, NULL);
		if( RET_WAS_ERROR(r) ) {
			checksums_free(readchecksums);
			deletefile(tempfilename);
			free(tempfilename);
			return r;
		}
	}
	checksums_free(readchecksums);
	file->tempfilename = tempfilename;
	file->used = true;
	return RET_OK;
}

static inline retvalue getsectionprioritycomponent(const struct incoming *i,const struct candidate *c,const struct distribution *into,const struct candidate_file *file, const char *name, const struct overrideinfo *oinfo, /*@out@*/const char **section_p, /*@out@*/const char **priority_p, /*@out@*/char **component) {
	retvalue r;
	const char *section, *priority;

	section = override_get(oinfo, SECTION_FIELDNAME);
	if( section == NULL ) {
		// TODO: warn about disparities here?
		section = file->section;
	}
	if( section == NULL || strcmp(section,"-") == 0 ) {
		fprintf(stderr, "No section found for '%s' ('%s' in '%s')!\n",
				name,
				BASENAME(i,file->ofs), BASENAME(i,c->ofs));
		return RET_ERROR;
	}
	priority = override_get(oinfo, PRIORITY_FIELDNAME);
	if( priority == NULL ) {
		// TODO: warn about disparities here?
		priority = file->priority;
	}
	if( priority == NULL || strcmp(priority,"-") == 0 ) {
		fprintf(stderr, "No priority found for '%s' ('%s' in '%s')!\n",
				name,
				BASENAME(i,file->ofs), BASENAME(i,c->ofs));
		return RET_ERROR;
	}

	r = guess_component(into->codename,&into->components,BASENAME(i,file->ofs),section,NULL,component);
	if( RET_WAS_ERROR(r) ) {
		return r;
	}
	*section_p = section;
	*priority_p = priority;
	return RET_OK;
}

static retvalue candidate_read_deb(struct incoming *i,struct candidate *c,struct candidate_file *file) {
	retvalue r;

	r = binaries_readdeb(&file->deb, file->tempfilename, true);
	if( RET_WAS_ERROR(r) )
		return r;
	if( strcmp(file->name, file->deb.name) != 0 ) {
		// TODO: add permissive thing to ignore this
		fprintf(stderr, "Name part of filename ('%s') and name within the file ('%s') do not match for '%s' in '%s'!\n",
				file->name, file->deb.name,
				BASENAME(i,file->ofs), BASENAME(i,c->ofs));
		return RET_ERROR;
	}
	if( strcmp(file->architecture, file->deb.architecture) != 0 ) {
		// TODO: add permissive thing to ignore this in some cases
		// but do not forget to look into into->architectures then
		fprintf(stderr, "Architecture '%s' of '%s' does not match '%s' specified in '%s'!\n",
				file->deb.architecture, BASENAME(i,file->ofs),
				file->architecture, BASENAME(i,c->ofs));
		return RET_ERROR;
	}
	if( strcmp(c->source, file->deb.source) != 0 ) {
		// TODO: add permissive thing to ignore this
		// (beware if tracking is active)
		fprintf(stderr, "Source header '%s' of '%s' and source name '%s' within the file '%s' do not match!\n",
				c->source, BASENAME(i,c->ofs),
				file->deb.source, BASENAME(i,file->ofs));
		return RET_ERROR;
	}
	if( strcmp(c->sourceversion, file->deb.sourceversion) != 0 ) {
		// TODO: add permissive thing to ignore this
		// (beware if tracking is active)
		fprintf(stderr, "Source version '%s' of '%s' and source version '%s' within the file '%s' do not match!\n",
				c->sourceversion, BASENAME(i,c->ofs),
				file->deb.sourceversion, BASENAME(i,file->ofs));
		return RET_ERROR;
	}
	if( ! strlist_in(&c->binaries, file->deb.name) ) {
		fprintf(stderr, "Name '%s' of binary '%s' is not listed in Binaries header of '%s'!\n",
				file->deb.name, BASENAME(i,file->ofs),
				BASENAME(i,c->ofs));
		return RET_ERROR;
	}
	r = properpackagename(file->deb.name);
	if( RET_IS_OK(r) )
		r = propersourcename(file->deb.source);
	if( RET_IS_OK(r) )
		r = properversion(file->deb.version);
	if( RET_IS_OK(r) )
		r = properfilenamepart(file->deb.architecture);
	if( RET_WAS_ERROR(r) )
		return r;
	return RET_OK;
}

static retvalue candidate_read_dsc(struct incoming *i, struct candidate_file *file) {
	retvalue r;
	bool broken = false;
	char *p;

	r = sources_readdsc(&file->dsc, file->tempfilename,
			BASENAME(i, file->ofs), &broken);
	if( RET_WAS_ERROR(r) )
		return r;
	p = calc_source_basename(file->dsc.name,
			file->dsc.version);
	if( p == NULL )
		return RET_ERROR_OOM;
	r = checksumsarray_include(&file->dsc.files, p, file->checksums);
	if( RET_WAS_ERROR(r) ) {
		return r;
	}
	// TODO: take a look at "broken"...
	return RET_OK;
}

static retvalue candidate_read_files(struct incoming *i, struct candidate *c) {
	struct candidate_file *file;
	retvalue r;

	for( file = c->files ; file != NULL ; file = file->next ) {
		if( file->section != NULL &&
				strcmp(file->section, "byhand") == 0 ) {
			/* to avoid further tests for this file */
			file->type = fe_UNKNOWN;
			continue;
		}
		if( !FE_PACKAGE(file->type) )
			continue;
		r = candidate_usefile(i, c, file);
		if( RET_WAS_ERROR(r) )
			return r;
		assert(file->tempfilename != NULL);

		if( FE_BINARY(file->type) )
			r = candidate_read_deb(i, c, file);
		else if( file->type == fe_DSC )
			r = candidate_read_dsc(i, file);
		else {
			r = RET_ERROR;
			assert( FE_BINARY(file->type) || file->type == fe_DSC );
		}
		if( RET_WAS_ERROR(r) )
			return r;
	}
	return RET_OK;
}

static retvalue candidate_preparechangesfile(struct database *database,const struct candidate *c,struct candidate_perdistribution *per) {
	retvalue r;
	char *basename, *filekey;
	struct candidate_package *package;
	struct candidate_file *file;
	const char *component = NULL;
	assert( c->files != NULL && c->files->ofs == c->ofs );

	/* search for a component to use */
	for( package = per->packages ; package != NULL ; package = package->next ) {
		if( package->component != NULL ) {
			component = package->component;
			break;
		}
	}
	if( component == NULL )
		component = "strange";

	file = changesfile(c);

	/* make sure the file is already copied */
	assert( file->used );
	assert( file->checksums != NULL );

	/* pseudo package containing the .changes file */
	package = candidate_newpackage(per, c->files);
	if( package == NULL )
		return RET_ERROR_OOM;

	basename = calc_changes_basename(c->source, c->changesversion, &c->architectures);
	if( basename == NULL )
		return RET_ERROR_OOM;

	filekey = calc_filekey(component, c->source, basename);
	free(basename);
	if( filekey == NULL )
		return RET_ERROR_OOM;

	r = strlist_init_singleton(filekey, &package->filekeys);
	if( RET_WAS_ERROR(r) )
		return r;
	assert( package->filekeys.count == 1 );
	filekey = package->filekeys.values[0];
	package->files = calloc(1, sizeof(struct candidate_file *));
	if( package->files == NULL )
		return RET_ERROR_OOM;
	r = files_canadd(database, filekey, file->checksums);
	if( RET_WAS_ERROR(r) )
		return r;
	if( RET_IS_OK(r) )
		package->files[0] = file;
	return RET_OK;
}

static retvalue prepare_deb(struct database *database,const struct incoming *i,const struct candidate *c,struct candidate_perdistribution *per,const struct candidate_file *file) {
	const char *section,*priority, *filekey;
	const struct overrideinfo *oinfo;
	struct candidate_package *package;
	const struct distribution *into = per->into;
	retvalue r;

	assert( FE_BINARY(file->type) );
	assert( file->tempfilename != NULL );
	assert( file->deb.name != NULL );

	package = candidate_newpackage(per, file);
	if( package == NULL )
		return RET_ERROR_OOM;
	assert( file == package->master );

	oinfo = override_search(file->type==fe_UDEB?into->overrides.udeb:
			                    into->overrides.deb,
	                        file->name);

	r = getsectionprioritycomponent(i,c,into,file,
			file->name, oinfo,
			&section, &priority, &package->component);
	if( RET_WAS_ERROR(r) )
		return r;

	if( file->type == fe_UDEB &&
	    !strlist_in(&into->udebcomponents, package->component)) {
		fprintf(stderr,
"Cannot put file '%s' of '%s' into component '%s',\n"
"as it is not listed in UDebComponents of '%s'!\n",
			BASENAME(i,file->ofs), BASENAME(i,c->ofs),
			package->component, into->codename);
		return RET_ERROR;
	}
	r = binaries_calcfilekeys(package->component, &file->deb,
			(package->master->type==fe_DEB)?"deb":"udeb",
			&package->filekeys);
	if( RET_WAS_ERROR(r) )
		return r;
	assert( package->filekeys.count == 1 );
	filekey = package->filekeys.values[0];
	package->files = calloc(1, sizeof(struct candidate_file *));
	if( package->files == NULL )
		return RET_ERROR_OOM;
	r = files_canadd(database, filekey, file->checksums);
	if( RET_WAS_ERROR(r) )
		return r;
	if( RET_IS_OK(r) )
		package->files[0] = file;
	r = binaries_complete(&file->deb, filekey, file->checksums, oinfo,
			section, priority, &package->control);
	if( RET_WAS_ERROR(r) )
		return r;
	return RET_OK;
}

static retvalue prepare_source_file(struct database *database, const struct incoming *i, const struct candidate *c, const char *filekey, const char *basename, struct checksums **checksums_p, int package_ofs, /*@out@*/const struct candidate_file **foundfile_p){
	struct candidate_file *f;
	const struct checksums * const checksums = *checksums_p;
	retvalue r;
	bool improves;

	f = c->files;
	while( f != NULL && (f->checksums == NULL ||
				strcmp(BASENAME(i, f->ofs), basename) != 0) )
		f = f->next;

	if( f == NULL ) {
		r = files_canadd(database, filekey, checksums);
		if( !RET_IS_OK(r) )
			return r;
		/* no file by this name and also no file with these
		 * characteristics in the pool, look for differently-named
		 * file with the same characteristics: */

		f = c->files;
		while( f != NULL && ( f->checksums == NULL ||
					!checksums_check(f->checksums,
						checksums, NULL)))
			f = f->next;

		if( f == NULL ) {
			fprintf(stderr, "file '%s' is needed for '%s', not yet registered in the pool and not found in '%s'\n",
					basename, BASENAME(i, package_ofs),
					BASENAME(i, c->ofs));
			return RET_ERROR;
		}
		/* otherwise proceed with the found file: */
	}

	if( !checksums_check(f->checksums, checksums, &improves) ) {
		fprintf(stderr, "file '%s' has conflicting checksums listed in '%s' and '%s'!\n",
				basename,
				BASENAME(i, c->ofs),
				BASENAME(i, package_ofs));
		return RET_ERROR;
	}
	if( improves ) {
		/* put additional checksums from the .dsc to the information
		 * found in .changes, so that a file matching those in .changes
		 * but not in .dsc is detected */
		r = checksums_combine(&f->checksums, checksums, NULL);
		assert( r != RET_NOTHING );
		if( RET_WAS_ERROR(r) )
			return r;
	}
	r = files_canadd(database, filekey, f->checksums);
	if( r == RET_NOTHING ) {
		/* already in the pool, mark as used (in the sense
		 * of "only not needed because it is already there") */
		f->used = true;

	} else if( RET_IS_OK(r) ) {
		/* don't have this file in the pool, make sure it is ready
		 * here */

		r = candidate_usefile(i, c, f);
		if( RET_WAS_ERROR(r) )
			return r;
		// TODO: update checksums to now received checksums?
		*foundfile_p = f;
	}
	if( !RET_WAS_ERROR(r) && !checksums_iscomplete(checksums) ) {
		/* update checksums so the source index can show them */
		r = checksums_combine(checksums_p, f->checksums, NULL);
	}
	return r;
}

static retvalue prepare_dsc(struct database *database,const struct incoming *i,const struct candidate *c,struct candidate_perdistribution *per,const struct candidate_file *file) {
	const char *section,*priority;
	const struct overrideinfo *oinfo;
	struct candidate_package *package;
	const struct distribution *into = per->into;
	retvalue r;
	int j;

	assert( file->type == fe_DSC );
	assert( file->tempfilename != NULL );
	assert( file->dsc.name != NULL );

	package = candidate_newpackage(per, file);
	if( package == NULL )
		return RET_ERROR_OOM;
	assert( file == package->master );

	if( c->isbinNMU ) {
		// TODO: add permissive thing to ignore this
		fprintf(stderr, "Source package ('%s') in '%s', which look like a binNMU (as '%s' and '%s' differ)!\n",
				BASENAME(i,file->ofs), BASENAME(i,c->ofs),
				c->sourceversion, c->changesversion);
		return RET_ERROR;
	}

	if( strcmp(file->name, file->dsc.name) != 0 ) {
		// TODO: add permissive thing to ignore this
		fprintf(stderr, "Name part of filename ('%s') and name within the file ('%s') do not match for '%s' in '%s'!\n",
				file->name, file->dsc.name,
				BASENAME(i,file->ofs), BASENAME(i,c->ofs));
		return RET_ERROR;
	}
	if( strcmp(c->source, file->dsc.name) != 0 ) {
		// TODO: add permissive thing to ignore this
		// (beware if tracking is active)
		fprintf(stderr, "Source header '%s' of '%s' and name '%s' within the file '%s' do not match!\n",
				c->source, BASENAME(i,c->ofs),
				file->dsc.name, BASENAME(i,file->ofs));
		return RET_ERROR;
	}
	if( strcmp(c->sourceversion, file->dsc.version) != 0 ) {
		// TODO: add permissive thing to ignore this
		// (beware if tracking is active)
		fprintf(stderr, "Source version '%s' of '%s' and version '%s' within the file '%s' do not match!\n",
				c->sourceversion, BASENAME(i,c->ofs),
				file->dsc.version, BASENAME(i,file->ofs));
		return RET_ERROR;
	}
	r = propersourcename(file->dsc.name);
	if( RET_IS_OK(r) )
		r = properversion(file->dsc.version);
	if( RET_IS_OK(r) )
		r = properfilenames(&file->dsc.files.names);
	if( RET_WAS_ERROR(r) )
		return r;
	oinfo = override_search(into->overrides.dsc, file->dsc.name);

	r = getsectionprioritycomponent(i, c, into, file,
			file->dsc.name, oinfo,
			&section, &priority, &package->component);
	if( RET_WAS_ERROR(r) )
		return r;
	package->directory = calc_sourcedir(package->component, file->dsc.name);
	if( package->directory == NULL )
		return RET_ERROR_OOM;
	r = calc_dirconcats(package->directory, &file->dsc.files.names, &package->filekeys);
	if( RET_WAS_ERROR(r) )
		return r;
	package->files = calloc(package->filekeys.count,sizeof(struct candidate *));
	if( package->files == NULL )
		return RET_ERROR_OOM;
	r = files_canadd(database, package->filekeys.values[0], file->checksums);
	if( RET_IS_OK(r) )
		package->files[0] = file;
	if( RET_WAS_ERROR(r) )
		return r;
	for( j = 1 ; j < package->filekeys.count ; j++ ) {
		r = prepare_source_file(database, i, c,
				package->filekeys.values[j],
				file->dsc.files.names.values[j],
				&file->dsc.files.checksums[j],
				file->ofs, &package->files[j]);
		if( RET_WAS_ERROR(r) )
			return r;
	}
	r = sources_complete(&file->dsc, package->directory, oinfo, section, priority, &package->control);
	if( RET_WAS_ERROR(r) )
		return r;

	return RET_OK;
}

static retvalue prepare_for_distribution(struct database *database,const struct incoming *i,const struct candidate *c,struct candidate_perdistribution *d) {
	struct candidate_file *file;
	retvalue r;

	d->into->lookedat = true;

	for( file = c->files ; file != NULL ; file = file->next ) {
		switch( file->type ) {
			case fe_UDEB:
			case fe_DEB:
				r = prepare_deb(database, i, c, d, file);
				break;
			case fe_DSC:
				r = prepare_dsc(database, i, c, d, file);
				break;
			default:
				r = RET_NOTHING;
				break;
		}
		if( RET_WAS_ERROR(r) ) {
			return r;
		}
	}
	if( d->into->tracking != dt_NONE ) {
		if( d->into->trackingoptions.includechanges ) {
			r = candidate_preparechangesfile(database, c, d);
			if( RET_WAS_ERROR(r) )
				return r;
		}
	}
	//... check if something would be done ...
	return RET_OK;
}

static retvalue candidate_removefiles(struct database *database,struct candidate *c,struct candidate_perdistribution *stopat,struct candidate_package *stopatatstopat,int stopatatstopatatstopat) {
	int j;
	struct candidate_perdistribution *d;
	struct candidate_package *p;
	retvalue r;

	for( d = c->perdistribution ; d != NULL ; d = d->next ) {
		for( p = d->packages ; p != NULL ; p = p->next ) {
			for( j = 0 ; j < p->filekeys.count ; j++ ) {
				if( d == stopat &&
				    p == stopatatstopat &&
				    j >= stopatatstopatatstopat )
					return RET_OK;

				if(  p->files[j] == NULL )
					continue;
				r = files_deleteandremove(database,
						p->filekeys.values[j],
						true, true);
				if( RET_WAS_ERROR(r) )
					return r;
			}
		}
	}
	return RET_OK;
}

static retvalue candidate_addfiles(struct database *database,struct candidate *c) {
	int j;
	struct candidate_perdistribution *d;
	struct candidate_package *p;
	retvalue r;

	for( d = c->perdistribution ; d != NULL ; d = d->next ) {
		for( p = d->packages ; p != NULL ; p = p->next ) {
			if( p->skip )
				continue;
			for( j = 0 ; j < p->filekeys.count ; j++ ) {
				const struct candidate_file *f = p->files[j];
				if(  f == NULL )
					continue;
				assert(f->tempfilename != NULL);
				r = files_hardlinkandadd(database,
						f->tempfilename,
						p->filekeys.values[j],
						f->checksums);
				if( !RET_IS_OK(r) )
					/* when we did not add it, do not remove it: */
					p->files[j] = NULL;
				if( RET_WAS_ERROR(r) ) {
					(void)candidate_removefiles(database,
							c, d, p, j);
					return r;
				}
			}
		}
	}
	return RET_OK;
}

static retvalue add_dsc(struct database *database,
		struct distribution *into, struct strlist *dereferenced,
		struct trackingdata *trackingdata, struct candidate_package *p) {
	retvalue r;
	struct target *t = distribution_getpart(into, p->component, "source", "dsc");
	// TODO: use this information:
	bool usedmarker = false;

	assert( logger_isprepared(into->logger) );

	/* finally put it into the source distribution */
	r = target_initpackagesdb(t, database, READWRITE);
	if( !RET_WAS_ERROR(r) ) {
		retvalue r2;
		if( interrupted() )
			r = RET_ERROR_INTERRUPTED;
		else
			r = target_addpackage(t, into->logger, database,
					p->master->dsc.name,
					p->master->dsc.version,
					p->control,
					&p->filekeys, &usedmarker,
					false, dereferenced,
					trackingdata, ft_SOURCE);
		r2 = target_closepackagesdb(t);
		RET_ENDUPDATE(r,r2);
	}
	RET_UPDATE(into->status, r);
	return r;
}

static retvalue checkadd_dsc(struct database *database,
		struct distribution *into,
		const struct incoming *i,
		bool tracking, struct candidate_package *p) {
	retvalue r;
	struct target *t = distribution_getpart(into, p->component, "source", "dsc");

	/* check for possible errors putting it into the source distribution */
	r = target_initpackagesdb(t, database, READONLY);
	if( !RET_WAS_ERROR(r) ) {
		retvalue r2;
		if( interrupted() )
			r = RET_ERROR_INTERRUPTED;
		else
			r = target_checkaddpackage(t,
					p->master->dsc.name,
					p->master->dsc.version,
					tracking, i->permit[pmf_oldpackagenewer]);
		r2 = target_closepackagesdb(t);
		RET_ENDUPDATE(r,r2);
	}
	return r;
}

static retvalue candidate_add_into(struct database *database,struct strlist *dereferenced,const struct incoming *i,const struct candidate *c,const struct candidate_perdistribution *d) {
	retvalue r;
	struct candidate_package *p;
	struct trackingdata trackingdata;
	struct distribution *into = d->into;
	trackingdb tracks;
	const char *changesfilekey = NULL;
	char *origfilename;

	if( interrupted() )
		return RET_ERROR_INTERRUPTED;

	into->lookedat = true;
	if( into->logger != NULL ) {
		r = logger_prepare(d->into->logger);
		if( RET_WAS_ERROR(r) )
			return r;
	}

	tracks = NULL;
	if( into->tracking != dt_NONE ) {
		r = tracking_initialize(&tracks, database, into, false);
		if( RET_WAS_ERROR(r) )
			return r;
	}
	if( tracks != NULL ) {
		r = trackingdata_summon(tracks, c->source, c->sourceversion,
				&trackingdata);
		if( RET_WAS_ERROR(r) ) {
			(void)tracking_done(tracks);
			return r;
		}
		if( into->trackingoptions.needsources ) {
			// TODO, but better before we start adding...
		}
	}

	origfilename = calc_dirconcat(i->directory,
			BASENAME(i, changesfile(c)->ofs));
	causingfile = origfilename;

	r = RET_OK;
	for( p = d->packages ; p != NULL ; p = p->next ) {
		if( p->skip ) {
			if( verbose >= 0 )
				printf(
"Not putting '%s' in '%s' as already in there with equal or newer version.\n",
					BASENAME(i,p->master->ofs),
					into->codename);
			continue;
		}
		if( p->master->type == fe_DSC ) {
			r = add_dsc(database, into, dereferenced,
					(tracks==NULL)?NULL:&trackingdata,
					p);
		} else if( FE_BINARY(p->master->type) ) {
			// TODO: use this information?
			bool usedmarker = false;
			r = binaries_adddeb(&p->master->deb, database,
					p->master->architecture,
					(p->master->type == fe_DEB)?"deb":"udeb",
					into, dereferenced,
					(tracks==NULL)?NULL:&trackingdata,
					p->component, &p->filekeys,
					&usedmarker, p->control);
		} else if( p->master->type == fe_UNKNOWN ) {
			/* finally add the .changes to tracking, if requested */
			assert( p->master->name == NULL );
			assert( tracks != NULL );

			r = trackedpackage_adddupfilekeys(trackingdata.tracks,
					trackingdata.pkg,
					ft_CHANGES, &p->filekeys, false, database);
			if( p->filekeys.count > 0 )
				changesfilekey = p->filekeys.values[0];
		} else
			r = RET_ERROR;

		if( RET_WAS_ERROR(r) ) {
			// TODO: remove files not yet referenced
			break;
		}
	}

	if( tracks != NULL ) {
		retvalue r2;
		r2 = trackingdata_finish(tracks, &trackingdata,
				database, dereferenced);
		RET_UPDATE(r,r2);
		r2 = tracking_done(tracks);
		RET_ENDUPDATE(r,r2);
	}
	if( RET_WAS_ERROR(r) ) {
		free(origfilename);
		return r;
	}
	logger_logchanges(into->logger, into->codename,
			c->source, c->changesversion, c->control,
			changesfile(c)->tempfilename, changesfilekey);
	free(origfilename);
	causingfile = NULL;
	return RET_OK;
}

static inline retvalue candidate_checkadd_into(struct database *database,const struct incoming *i,const struct candidate_perdistribution *d) {
	retvalue r;
	struct candidate_package *p;
	struct distribution *into = d->into;
	bool somethingtodo = false;

	for( p = d->packages ; p != NULL ; p = p->next ) {
		if( p->master->type == fe_DSC ) {
			r = checkadd_dsc(database, into,
					i, into->tracking != dt_NONE,
					p);
		} else if( FE_BINARY(p->master->type) ) {
			r = binaries_checkadddeb(&p->master->deb, database,
					p->master->architecture,
					(p->master->type == fe_DEB)?"deb":"udeb",
					into,
					into->tracking != dt_NONE,
					p->component, i->permit[pmf_oldpackagenewer]);
		} else if( p->master->type == fe_UNKNOWN ) {
			continue;
		} else
			r = RET_ERROR;

		if( RET_WAS_ERROR(r) )
			return r;
		if( r == RET_NOTHING ) 
			p->skip = true;
		else
			somethingtodo = true;
	}
	if( somethingtodo )
		return RET_OK;
	else
		return RET_NOTHING;
}

static inline bool isallowed(UNUSED(struct incoming *i), UNUSED(struct candidate *c), UNUSED(struct distribution *into), const struct uploadpermissions *permissions) {
	return permissions->allowall;
}

static retvalue candidate_checkpermissions(const char *confdir, struct incoming *i, struct candidate *c, struct distribution *into) {
	retvalue r;
	int j;

	/* no rules means allowed */
	if( into->uploaders == NULL )
		return RET_OK;

	r = distribution_loaduploaders(into, confdir);
	if( RET_WAS_ERROR(r) )
		return r;
	assert(into->uploaderslist != NULL);

	if( c->keys.count == 0 ) {
		const struct uploadpermissions *permissions;

		r = uploaders_unsignedpermissions(into->uploaderslist,
				&permissions);
		assert( r != RET_NOTHING );
		if( RET_WAS_ERROR(r) )
			return r;
		if( permissions != NULL && isallowed(i,c,into,permissions) )
			return RET_OK;
	} else for( j = 0; j < c->keys.count ; j++ ) {
		const struct uploadpermissions *permissions;

		r = uploaders_permissions(into->uploaderslist,
				c->keys.values[j], &permissions);
		assert( r != RET_NOTHING );
		if( RET_WAS_ERROR(r) )
			return r;
		if( permissions != NULL && isallowed(i,c,into,permissions) )
			return RET_OK;
	}
	/* reject, check if it would have been accepted with more signatures
	 * valid: */
	if( verbose >= 0 &&
	    c->allkeys.count != 0 && c->allkeys.count != c->keys.count ) {
		for( j = 0; j < c->allkeys.count ; j++ ) {
			const struct uploadpermissions *permissions;
			r = uploaders_permissions(into->uploaderslist,
					c->allkeys.values[j], &permissions);
			assert( r != RET_NOTHING );
			if( RET_WAS_ERROR(r) )
				return r;
			if( permissions != NULL &&
			    isallowed(i,c , into, permissions) ) {
				// TODO: get information if it was invalid because
				// missing pub-key, expire-state or something else
				// here so warning can be more specific.
				fprintf(stderr,
"'%s' would have been accepted into '%s' if signature with '%s' was checkable and valid.\n",
					i->files.values[c->ofs],
					into->codename,
					c->allkeys.values[j]);
			}
		}
	}

	/* reject */
	return RET_NOTHING;
}

static retvalue check_architecture_availability(const struct incoming *i, const struct candidate *c) {
	struct candidate_perdistribution *d;
	bool check_all_availability = false;
	bool have_all_available = false;
	int j;

	// TODO: switch to instead ensure every architecture can be put into
	// one distribution at least would be nice. If implementing this do not
	// forget to check later to only put files in when the distribution can
	// cope with that.

	for( j = 0 ; j < c->architectures.count ; j++ ) {
		const char *architecture = c->architectures.values[j];
		if( strcmp(architecture, "all") == 0 ) {
			check_all_availability = true;
			continue;
		}
		for( d = c->perdistribution ; d != NULL ; d = d->next ) {
			if( strlist_in(&d->into->architectures, architecture) )
				continue;
			fprintf(stderr, "'%s' lists architecture '%s' not found in distribution '%s'!\n",
					BASENAME(i,c->ofs), architecture, d->into->codename);
			return RET_ERROR;
		}
		if( strcmp(architecture, "source") != 0 )
			have_all_available = true;
	}
	if( check_all_availability && ! have_all_available ) {
		for( d = c->perdistribution ; d != NULL ; d = d->next ) {
			if( d->into->architectures.count > 1 )
				continue;
			if( d->into->architectures.count > 0 &&
				strcmp(d->into->architectures.values[0],"source") != 0)
				continue;
			fprintf(stderr, "'%s' lists architecture 'all' but not binary architecture found in distribution '%s'!\n",
					BASENAME(i,c->ofs), d->into->codename);
			return RET_ERROR;
		}
	}
	return RET_OK;
}

static retvalue candidate_add(const char *confdir, const char *overridedir,struct database *database, struct strlist *dereferenced, struct incoming *i, struct candidate *c) {
	struct candidate_perdistribution *d;
	struct candidate_file *file;
	retvalue r;
	bool somethingtodo;
	assert( c->perdistribution != NULL );

	/* check if every distribution this is to be added to supports
	 * all architectures we have files for */
	r = check_architecture_availability(i, c);
	if( RET_WAS_ERROR(r) )
		return r;

	for( d = c->perdistribution ; d != NULL ; d = d->next ) {
		r = distribution_loadalloverrides(d->into, confdir, overridedir);
		if( RET_WAS_ERROR(r) )
			return r;
	}

	// TODO: once uploaderlist allows to look for package names or existing override
	// entries or such things, check package names here enable checking for content
	// name with outer name

	/* when we get here, the package is allowed in, now we have to
	 * read the parts and check all stuff we only know now */

	r = candidate_read_files(i, c);
	if( RET_WAS_ERROR(r) )
		return r;

	/* now the distribution specific part starts: */
	for( d = c->perdistribution ; d != NULL ; d = d->next ) {
		r = prepare_for_distribution(database, i, c, d);
			if( RET_WAS_ERROR(r) ) {
				return r;
			}
	}
	for( file = c->files ; file != NULL ; file = file->next ) {
		if( !file->used && !i->permit[pmf_unused_files] ) {
			// TODO: find some way to mail such errors...
			fprintf(stderr,
"Error: '%s' contains unused file '%s'!\n"
"(Do Permit: unused_files to conf/incoming to ignore and\n"
" additionaly Cleanup: unused_files to delete them)\n",
				BASENAME(i,c->ofs), BASENAME(i,file->ofs));
			return RET_ERROR;
		}
	}

	/* additional test run to see if anything could go wrong,
	 * or if there are already newer versions */
	somethingtodo = false;
	for( d = c->perdistribution ; d != NULL ; d = d->next ) {
		r = candidate_checkadd_into(database,
			i, d);
		if( RET_WAS_ERROR(r) )
			return r;
		if( r == RET_NOTHING )
			d->skip = true;
		else
			somethingtodo = true;
	}
	if( ! somethingtodo ) {
		if( verbose >= 0 ) {
			printf("Skipping %s because all packages are skipped!\n",
					BASENAME(i,c->ofs));
		}
		for( file = c->files ; file != NULL ; file = file->next ) {
			if( file->used || i->cleanup[cuf_unused_files] )
				i->delete[file->ofs] = true;
		}
		return RET_NOTHING;
	}

	// TODO: make sure not two different files are supposed to be installed
	// as the same filekey.

	/* the actual adding of packages, make sure what can be checked was
	 * checked by now */

	/* make hardlinks/copies of the files */
	r = candidate_addfiles(database, c);
	if( RET_WAS_ERROR(r) )
		return r;
	if( interrupted() ) {
		(void)candidate_removefiles(database,c,NULL,NULL,0);
		return RET_ERROR_INTERRUPTED;
	}
	r = RET_OK;
	for( d = c->perdistribution ; d != NULL ; d = d->next ) {
		if( d->skip )
			continue;
		r = candidate_add_into(database,
			dereferenced, i, c, d);
		if( RET_WAS_ERROR(r) )
			return r;
	}

	/* mark files as done */
	for( file = c->files ; file != NULL ; file = file->next ) {
		if( file->used || i->cleanup[cuf_unused_files] ) {
			i->delete[file->ofs] = true;
		}
	}
	return RET_OK;
}

static retvalue process_changes(const char *confdir,const char *overridedir,struct database *database,struct strlist *dereferenced,struct incoming *i,int ofs) {
	struct candidate *c;
	struct candidate_file *file;
	retvalue r;
	int j,k;
	bool broken = false, tried = false;

	r = candidate_read(i, ofs, &c, &broken);
	if( RET_WAS_ERROR(r) )
		return r;
	assert( RET_IS_OK(r) );
	r = candidate_parse(i, c);
	if( RET_WAS_ERROR(r) ) {
		candidate_free(c);
		return r;
	}
	r = candidate_earlychecks(i, c);
	if( RET_WAS_ERROR(r) ) {
		candidate_free(c);
		return r;
	}
	for( k = 0 ; k < c->distributions.count ; k++ ) {
		const char *name = c->distributions.values[k];

		for( j = 0 ; j < i->allow.count ; j++ ) {
			// TODO: implement "*"
			if( strcmp(name, i->allow.values[j]) == 0 ) {
				tried = true;
				r = candidate_checkpermissions(confdir, i, c,
						i->allow_into[j]);
				if( r == RET_NOTHING )
					continue;
				if( RET_IS_OK(r) )
					r = candidate_newdistribution(c,
							i->allow_into[j]);
				if( RET_WAS_ERROR(r) ) {
					candidate_free(c);
					return r;
				} else
					break;
			}
		}
		if( c->perdistribution != NULL &&
				!i->permit[pmf_multiple_distributions] )
			break;
	}
	if( c->perdistribution == NULL && i->default_into != NULL ) {
		tried = true;
		r = candidate_checkpermissions(confdir, i, c, i->default_into);
		if( RET_WAS_ERROR(r) ) {
			candidate_free(c);
			return r;
		}
		if( RET_IS_OK(r) ) {
			r = candidate_newdistribution(c, i->default_into);
		}
	}
	if( c->perdistribution == NULL ) {
		fprintf(stderr, tried?"No distribution accepting '%s'!\n":
				      "No distribution found for '%s'!\n",
			i->files.values[ofs]);
		if( i->cleanup[cuf_on_deny]  ) {
			i->delete[c->ofs] = true;
			for( file = c->files ; file != NULL ; file = file->next ) {
				// TODO: implement same-owner check
				if( !i->cleanup[cuf_on_deny_check_owner] )
					i->delete[file->ofs] = true;
			}
		}
		r = RET_ERROR;
	} else {
		if( broken ) {
			fprintf(stderr,
"'%s' is signed with only invalid signatures.\n"
"If this was not corruption but willfull modification,\n"
"remove the signatures and try again.\n",
				i->files.values[ofs]);
			r = RET_ERROR;
		} else
			r = candidate_add(confdir, overridedir, database,
					dereferenced,
					i, c);
		if( RET_WAS_ERROR(r) && i->cleanup[cuf_on_error] ) {
			struct candidate_file *file;

			i->delete[c->ofs] = true;
			for( file = c->files ; file != NULL ; file = file->next ) {
				i->delete[file->ofs] = true;
			}
		}
	}
	logger_wait();
	candidate_free(c);
	return r;
}

/* tempdir should ideally be on the same partition like the pooldir */
retvalue process_incoming(const char *basedir,const char *confdir,const char *overridedir,struct database *database,struct strlist *dereferenced,struct distribution *distributions,const char *name,const char *changesfilename) {
	struct incoming *i;
	retvalue result,r;
	int j;

	result = RET_NOTHING;

	r = incoming_init(basedir, confdir, distributions, name, &i);
	if( RET_WAS_ERROR(r) )
		return r;

	for( j = 0 ; j < i->files.count ; j ++ ) {
		const char *basename = i->files.values[j];
		size_t l = strlen(basename);
#define C_SUFFIX ".changes"
#define C_LEN strlen(C_SUFFIX)
		if( l <= C_LEN || strcmp(basename+(l-C_LEN),C_SUFFIX) != 0 )
			continue;
		if( changesfilename != NULL && strcmp(basename, changesfilename) != 0 )
			continue;
		/* a .changes file, check it */
		r = process_changes(confdir, overridedir, database, dereferenced, i, j);
		RET_UPDATE(result, r);
	}

	logger_wait();
	for( j = 0 ; j < i->files.count ; j ++ ) {
		char *fullfilename;

		if( !i->delete[j] )
			continue;

		fullfilename = calc_dirconcat(i->directory, i->files.values[j]);
		if( fullfilename == NULL ) {
			result = RET_ERROR_OOM;
			continue;
		}
		if( verbose >= 3 )
			printf("deleting '%s'...\n", fullfilename);
		deletefile(fullfilename);
		free(fullfilename);
	}
	incoming_free(i);
	return result;
}
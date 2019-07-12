/* CTF linking.
   Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.

   Licensed under the Universal Permissive License v 1.0 as shown at
   http://oss.oracle.com/licenses/upl.

   Licensed under the GNU General Public License (GPL), version 2. See the file
   COPYING in the top level of this tree.  */

#include <ctf-impl.h>
#include <string.h>

/* Type tracking machinery.  */

DECL_CTF_HASH_SIZED (ctf_link_type_mapping_key_t)

/* Record the correspondence between a source and ctf_add_type()-added
   destination type: both types are translated into parent type IDs if need be,
   so they relate to the actual container they are in.  Outside controlled
   circumstances (like linking) it is probably not useful to do more than
   compare these pointers, since there is nothing stopping the user closing the
   source container whenever they want to.

   Our OOM handling here is just to not do anything, because this is called deep
   enough in the call stack that doing anything useful is painfully difficult:
   the worst consequence if we do OOM is a bit of type duplication anyway.  */

void
ctf_add_type_mapping (ctf_file_t *src_fp, ctf_id_t src_type,
		      ctf_file_t *dst_fp, ctf_id_t dst_type)
{
  if (LCTF_TYPE_ISPARENT (src_fp, src_type) && src_fp->ctf_parent)
    src_fp = src_fp->ctf_parent;

  src_type = LCTF_TYPE_TO_INDEX(src_fp, src_type);

  if (LCTF_TYPE_ISPARENT (dst_fp, dst_type) && dst_fp->ctf_parent)
    dst_fp = dst_fp->ctf_parent;

  dst_type = LCTF_TYPE_TO_INDEX(dst_fp, dst_type);

  /* This dynhash is a bit tricky: it has a multivalued (structural) key, so we
     need to use the sized-hash machinery to generate key hashing and equality
     functions.  */

  if (dst_fp->ctf_link_type_mapping == NULL)
    {
      ctf_hash_fun f = CTF_HASH_SIZED (ctf_link_type_mapping_key_t);
      ctf_hash_eq_fun e = CTF_HASH_EQ_SIZED (ctf_link_type_mapping_key_t);

      if ((dst_fp->ctf_link_type_mapping = ctf_dynhash_create (f, e, free,
							       NULL)) == NULL)
	return;
    }

  ctf_link_type_mapping_key_t *key;
  key = calloc (1, sizeof (struct ctf_link_type_mapping_key));
  if (!key)
    return;

  key->cltm_fp = src_fp;
  key->cltm_idx = src_type;

  ctf_dynhash_insert (dst_fp->ctf_link_type_mapping, key,
		      (void *) (uintptr_t) dst_type);
}

/* Look up a type mapping: return 0 if none.  The DST_FP is modified to point to
   the parent if need be.  The ID returned is from the dst_fp's perspective.  */
ctf_id_t
ctf_type_mapping (ctf_file_t *src_fp, ctf_id_t src_type, ctf_file_t **dst_fp)
{
  ctf_link_type_mapping_key_t key;
  ctf_file_t *target_fp = *dst_fp;
  ctf_id_t dst_type = 0;

  memset (&key, 0, sizeof (struct ctf_link_type_mapping_key));

  if (LCTF_TYPE_ISPARENT (src_fp, src_type) && src_fp->ctf_parent)
    src_fp = src_fp->ctf_parent;

  src_type = LCTF_TYPE_TO_INDEX(src_fp, src_type);
  key.cltm_fp = src_fp;
  key.cltm_idx = src_type;

  if (target_fp->ctf_link_type_mapping)
    dst_type = (uintptr_t) ctf_dynhash_lookup (target_fp->ctf_link_type_mapping,
					       &key);

  if (dst_type != 0)
    {
      dst_type = LCTF_INDEX_TO_TYPE (target_fp, dst_type,
				     target_fp->ctf_parent != NULL);
      *dst_fp = target_fp;
      return dst_type;
    }

  if (target_fp->ctf_parent)
    target_fp = target_fp->ctf_parent;
  else
    return 0;

  if (target_fp->ctf_link_type_mapping)
    dst_type = (uintptr_t) ctf_dynhash_lookup (target_fp->ctf_link_type_mapping,
					       &key);

  if (dst_type)
    dst_type = LCTF_INDEX_TO_TYPE (target_fp, dst_type,
				   target_fp->ctf_parent != NULL);

  *dst_fp = target_fp;
  return dst_type;
}

/* Linker machinery.

   CTF linking consists of adding CTF archives full of content to be merged into
   this one to the current file (which must be writable) by calling
   ctf_link_add_ctf().  Once this is done, a call to ctf_link() will merge the
   type tables together, generating new CTF files as needed, with this one as a
   parent, to contain types from the inputs which conflict.
   ctf_link_add_strtab() takes a callback which provides string/offset pairs to
   be added to the external symbol table and deduplicated from all CTF string
   tables in the output link; ctf_link_shuffle_syms() takes a callback which
   provides symtab entries in ascending order, and shuffles the function and
   data sections to match; and ctf_link_write() emits a CTF file (if there are
   no conflicts requiring per-compilation-unit sub-CTF files) or CTF archives
   (otherwise) and returns it, suitable for addition in the .ctf section of the
   output.  */

/* Add a file to a link.  */

static void ctf_arc_close_thunk (void *arc)
{
  ctf_arc_close ((ctf_archive_t *) arc);
}

static void ctf_file_close_thunk (void *file)
{
  ctf_file_close ((ctf_file_t *) file);
}

int
ctf_link_add_ctf (ctf_file_t *fp, ctf_archive_t *ctf, const char *name)
{
  char *dupname = NULL;

  if (fp->ctf_link_outputs)
    return (ctf_set_errno (fp, ECTF_LINKADDEDLATE));
  if (fp->ctf_link_inputs == NULL)
    fp->ctf_link_inputs = ctf_dynhash_create (ctf_hash_string,
					      ctf_hash_eq_string, free,
					      ctf_arc_close_thunk);

  if (fp->ctf_link_inputs == NULL)
    goto oom;

  if ((dupname = strdup (name)) == NULL)
    goto oom;

  if (ctf_dynhash_insert (fp->ctf_link_inputs, dupname, ctf) < 0)
    goto oom;

  return 0;
 oom:
  free (fp->ctf_link_inputs);
  fp->ctf_link_inputs = NULL;
  free (dupname);
  return (ctf_set_errno (fp, ENOMEM));
}

typedef struct ctf_link_in_member_cb_arg
{
  ctf_file_t *out_fp;
  const char *file_name;
  ctf_file_t *in_fp;
  ctf_file_t *main_input_fp;
  char *cu_name;
  char *arcname;
  int done_main_member;
  int share_mode;
  int in_input_cu_file;
  int err;
} ctf_link_in_member_cb_arg_t;


/* Link one type into the link.  We rely on ctf_add_type() to detect
   duplicates.  This is not terribly reliable yet (unnmamed types will be
   mindlessly duplicated), but will improve shortly.  */

static int
ctf_link_one_type (ctf_id_t type, void *arg_)
{
  ctf_link_in_member_cb_arg_t *arg = (ctf_link_in_member_cb_arg_t *) arg_;
  ctf_file_t *per_cu_out_fp;
  int err;

  if (arg->share_mode != CTF_LINK_SHARE_UNCONFLICTED)
    {
      ctf_dprintf ("Share-duplicated mode not yet implemented.\n");
      return ECTF_NOTYET;
    }

  /* Simply call ctf_add_type: if it reports a conflict and we're adding to the
     main CTF file, add to the per-CU archive member instead, creating it if
     necessary.  If we got this type from a per-CU archive member, add it
     straight back to the corresponding member in the output.  */

  if (!arg->in_input_cu_file)
    {
      err = ctf_add_type (arg->out_fp, arg->in_fp, type);

      if (err > -1)
	return 0;

      if (err != ECTF_CONFLICT)
	{
	  ctf_dprintf ("Cannot link type %lx from archive member %s, input file %s "
		       "into output link: %s\n", type, arg->arcname, arg->file_name,
		       ctf_errmsg (ctf_errno (arg->out_fp)));
	  return err;
	}
    }

  if ((per_cu_out_fp = ctf_dynhash_lookup (arg->out_fp->ctf_link_outputs,
					   arg->arcname)) == NULL)
    {
      int err;

      if ((per_cu_out_fp = ctf_create (&err)) == NULL)
	{
	  ctf_dprintf ("Cannot create per-CU CTF archive for member %s: %s\n",
		       arg->arcname, ctf_errmsg (err));
	  return err;
	}

      if (ctf_dynhash_insert (arg->out_fp->ctf_link_outputs, arg->arcname,
			      per_cu_out_fp) < 0)
	  return ENOMEM;

      ctf_import (per_cu_out_fp, arg->out_fp);
      ctf_cuname_set (per_cu_out_fp, arg->cu_name);
    }

  err = ctf_add_type (per_cu_out_fp, arg->in_fp, type);

  if (err > -1)
    return 0;

  ctf_dprintf ("Cannot link type %lx from CTF archive member %s, input file %s "
	       "into output per-CU CTF archive member %s: %s: skipped\n", type,
	       arg->arcname, arg->file_name, arg->arcname,
	       ctf_errmsg (ctf_errno (arg->out_fp)));
  return err;			/* Should be impossible: abort link.  */
}

/* Link one variable in.  */

static int
ctf_link_one_variable (const char *name, ctf_id_t type, void *arg_)
{
  ctf_link_in_member_cb_arg_t *arg = (ctf_link_in_member_cb_arg_t *) arg_;
  ctf_dvdef_t *dvd;
  ctf_id_t dst_type = 0;
  ctf_file_t *check_fp;

  /* In unconflicted link mode, when called on a child, we want to try to merge
     into the parent first, then the child (if there is one): it must be
     possible to merge into one of those given valid input.  Look for the type
     of this variable in the parent.  */

  if (arg->out_fp->ctf_parent)
    {
      check_fp = arg->out_fp->ctf_parent;

      dst_type = ctf_type_mapping (arg->in_fp, type, &check_fp);
      if (dst_type != 0)
	{
	  /* Got it in the parent.  Is there already a variable of this name in
	     the parent? Does it already refer to the right type?  */

	  dvd = ctf_dynhash_lookup (check_fp->ctf_dvhash, name);
	  if (dvd && dvd->dvd_type == dst_type)
	    return 0;

	  /* No variable here: we can add it.  */
	  if (!dvd)
	    {
	      ctf_add_variable (check_fp, name, dst_type);
	      return 0;
	    }
	}
    }

  /* Not in the parent, or conflicted, or no parent at all.  Find the type in
     the child if necessary, then add it there.  */

  /* This type is from the parent's perspective: childify it.  */
  if (dst_type != 0 && arg->out_fp->ctf_parent)
    {
      dst_type = LCTF_TYPE_TO_INDEX (arg->out_fp->ctf_parent, dst_type);
      dst_type = LCTF_INDEX_TO_TYPE (arg->out_fp, dst_type, 1);
    }
  else
    {
      /* Look up the type in the child.  */
      check_fp = arg->out_fp;

      dst_type = ctf_type_mapping (arg->in_fp, type, &check_fp);
    }

  /* Type still unknown. Impossible: warn and fail.  */
  if (dst_type == 0)
    {
      ctf_dprintf ("Type %lx from CTF archive member %s, input file %s not "
		   "known in parent while adding variable %s: this should "
		   "never happen.\n", type, arg->arcname, arg->file_name,
		   name);
      return EINVAL;
    }

  ctf_add_variable (check_fp, name, dst_type);

  return 0;
}

/* Merge every type and variable in this archive member into the link, so we can
   relink things that have already had ld run on them.  We use the archive
   member name, sans any leading '.ctf.', as the CU name for ambiguous types if
   there is one and it's not the default: otherwise, we use the name of the
   input file.  */
static int
ctf_link_one_input_archive_member (ctf_file_t *in_fp, const char *name, void *arg_)
{
  ctf_link_in_member_cb_arg_t *arg = (ctf_link_in_member_cb_arg_t *) arg_;
  int err;

  if (strcmp (name, _CTF_SECTION) == 0)
    {
      /* This file is the default member of this archive, and has already been
	 explicitly processed.

	 In the default sharing mode of CTF_LINK_SHARE_UNCONFLICTED, it does no
	 harm to rescan an existing shared repo again: all the types will just
	 end up in the same place.  But in CTF_LINK_SHARE_DUPLICATED mode, this
	 causes the system to erroneously conclude that all types are duplicated
	 and should be shared, even if they are not.  */

      if (arg->done_main_member)
	return 0;
      arg->arcname = strdup (".ctf.");
      arg->arcname = ctf_str_append (arg->arcname, arg->file_name);
    }
  else
    {
      arg->arcname = strdup (name);

      /* Get ambiguous types from our parent.  */
      ctf_import (in_fp, arg->main_input_fp);
      arg->in_input_cu_file = 1;
    }

  arg->cu_name = arg->arcname;
  if (strncmp (arg->cu_name, ".ctf.", strlen (".ctf.")) == 0)
    arg->cu_name += strlen (".ctf.");
  arg->in_fp = in_fp;

  err = ctf_type_iter_all (in_fp, ctf_link_one_type, arg);

  if (err == 0)
    err = ctf_variable_iter (in_fp, ctf_link_one_variable, arg);
  arg->in_input_cu_file = 0;
  free (arg->arcname);

  return err;
}

/* Link one input file's types into the output file.  */
static void
ctf_link_one_input_archive (void *key, void *value, void *arg_)
{
  const char *file_name = (const char *) key;
  ctf_archive_t *arc = (ctf_archive_t *) value;
  ctf_link_in_member_cb_arg_t *arg = (ctf_link_in_member_cb_arg_t *) arg_;
  int err;

  arg->file_name = file_name;
  arg->done_main_member = 0;
  if ((arg->main_input_fp = ctf_arc_open_by_name (arc, NULL, &err)) == NULL)
    if (err != ECTF_ARNNAME)
      {
	ctf_dprintf ("Cannot open main archive member in input file %s in the "
		     "link: skipping: %s.\n", arg->file_name,
		     ctf_errmsg (err));
	return;
      }

  ctf_link_one_input_archive_member (arg->main_input_fp, _CTF_SECTION, arg);
  arg->done_main_member = 1;
  if ((err = ctf_archive_iter (arc, ctf_link_one_input_archive_member,
			       arg)) < 0)
    {
      ctf_dprintf ("Cannot traverse archive in input file %s: some types "
		   "skipped: %s.\n", arg->file_name, ctf_errmsg (err));
      arg->err = err;
    }
  ctf_file_close (arg->main_input_fp);
}

/* Merge types and variable sections in all files added to the link
   together.  */
int
ctf_link (ctf_file_t *fp, int share_mode)
{
  ctf_link_in_member_cb_arg_t arg;

  memset (&arg, 0, sizeof (struct ctf_link_in_member_cb_arg));
  arg.out_fp = fp;
  arg.share_mode = share_mode;

  if (fp->ctf_link_inputs == NULL)
    return 0;					/* Nothing to do. */

  if (fp->ctf_link_outputs == NULL)
    fp->ctf_link_outputs = ctf_dynhash_create (ctf_hash_string,
					       ctf_hash_eq_string, free,
					       ctf_file_close_thunk);

  if (fp->ctf_link_outputs == NULL)
    return ctf_set_errno (fp, ENOMEM);

  ctf_dynhash_iter (fp->ctf_link_inputs, ctf_link_one_input_archive,
		    &arg);

  /* Promote any sub-CU errors into the main archive.  */
  if (arg.err)
    return ctf_set_errno (fp, arg.err);
  return 0;
}

typedef struct ctf_link_out_string_cb_arg
{
  const char *str;
  uint32_t offset;
  int err;
} ctf_link_out_string_cb_arg_t;

/* Intern a string in the string table of an output per-CU CTF file.  */
static void
ctf_link_intern_extern_string (void *key _libctf_unused_, void *value,
			       void *arg_)
{
  ctf_file_t *fp = (ctf_file_t *) value;
  ctf_link_out_string_cb_arg_t *arg = (ctf_link_out_string_cb_arg_t *) arg_;

  fp->ctf_flags |= LCTF_DIRTY;
  if (ctf_str_add_external (fp, arg->str, arg->offset) == NULL)
    arg->err = ENOMEM;
}

/* Repeatedly call ADD_STRING to acquire strings from the external string table,
   adding them to the atoms table for this CU and all subsidiary CUs.

   If ctf_link() is also called, it must be called first if you want the new CTF
   files ctf_link() can create to get their strings dedupped against the ELF
   strtab properly.  */
int
ctf_link_add_strtab (ctf_file_t *fp, ctf_link_strtab_string_f *add_string,
		     void *arg)
{
  const char *str;
  uint32_t offset;
  int err = 0;

  while ((str = add_string (&offset, arg)) != NULL)
    {
      ctf_link_out_string_cb_arg_t iter_arg = { str, offset, 0 };

      fp->ctf_flags |= LCTF_DIRTY;
      if (ctf_str_add_external (fp, str, offset) == NULL)
	err = ENOMEM;

      ctf_dynhash_iter (fp->ctf_link_outputs, ctf_link_intern_extern_string,
			&iter_arg);
      if (iter_arg.err)
	err = iter_arg.err;
    }

  return err;
}

/* Not yet implemented.  */
int
ctf_link_shuffle_syms (ctf_file_t *fp _libctf_unused_,
		       ctf_link_iter_symbol_f *add_sym _libctf_unused_,
		       void *arg _libctf_unused_)
{
  return 0;
}

typedef struct ctf_name_list_accum_cb_arg
{
  char **names;
  ctf_file_t **files;
  size_t i;
  int err;
} ctf_name_list_accum_cb_arg_t;

/* Accumulate the names and a count of the names in the link output hash,
   and run ctf_update() on them to generate them.  */
static void
ctf_accumulate_archive_names (void *key, void *value, void *arg_)
{
  const char *name = (const char *) key;
  ctf_file_t *fp = (ctf_file_t *) value;
  char **names;
  ctf_file_t **files;
  ctf_name_list_accum_cb_arg_t *arg = (ctf_name_list_accum_cb_arg_t *) arg_;
  int err;

  if ((err = ctf_update (fp)) < 0)
    {
      arg->err = err;
      return;
    }

  if ((names = realloc (arg->names, sizeof (char *) * ++(arg->i))) == NULL)
    {
      (arg->i)--;
      arg->err = ENOMEM;
      return;
    }

  if ((files = realloc (arg->files, sizeof (ctf_file_t *) * arg->i)) == NULL)
    {
      (arg->i)--;
      arg->err = ENOMEM;
      return;
    }
  arg->names = names;
  arg->names[(arg->i) - 1] = (char *) name;
  arg->files = files;
  arg->files[(arg->i) - 1] = fp;
}

/* Write out a CTF archive (if there are per-CU CTF files) or a CTF file
   (otherwise) into a new dynamically-allocated string, and return it.
   Members with sizes above THRESHOLD are compressed.  */
unsigned char *
ctf_link_write (ctf_file_t *fp, size_t *size, size_t threshold)
{
  ctf_name_list_accum_cb_arg_t arg;
  char **names;
  ctf_file_t **files;
  FILE *f = NULL;
  int err;
  long fsize;
  const char *errloc;
  unsigned char *buf = NULL;

  memset (&arg, 0, sizeof (ctf_name_list_accum_cb_arg_t));

  if ((err = ctf_update (fp)) < 0)
    {
      errloc = "CTF file construction";
      goto err;
    }

  if (fp->ctf_link_outputs)
    {
      ctf_dynhash_iter (fp->ctf_link_outputs, ctf_accumulate_archive_names, &arg);
      if (arg.err)
	{
	  errloc = "hash creation";
	  err = arg.err;
	  goto err;
	}
    }

  /* No extra outputs? Just write a simple ctf_file_t.  */
  if (arg.i == 0)
    return ctf_write_mem (fp, size, threshold);

  /* Writing an archive.  Stick ourselves (the shared repository, parent of all
     other archives) on the front of it with the default name.  */
  if ((names = realloc (arg.names, sizeof (char *) * (arg.i + 1))) == NULL)
    {
      errloc = "name reallocation";
      goto err_no;
    }
  arg.names = names;
  memmove (&(arg.names[1]), arg.names, sizeof (char *) * (arg.i));
  arg.names[0] = (char *) _CTF_SECTION;

  if ((files = realloc (arg.files,
			sizeof (struct ctf_file *) * (arg.i + 1))) == NULL)
    {
      errloc = "ctf_file reallocation";
      goto err_no;
    }
  arg.files = files;
  memmove (&(arg.files[1]), arg.files, sizeof (ctf_file_t *) * (arg.i));
  arg.files[0] = fp;

  if ((f = tmpfile ()) == NULL)
    {
      errloc = "tempfile creation";
      goto err_no;
    }

  if ((err = ctf_arc_write_fd (fileno (f), arg.files, arg.i + 1,
			       (const char **) arg.names,
			       threshold)) < 0)
    {
      errloc = "archive writing";
      goto err;
    }

  if (fseek (f, 0, SEEK_END) < 0)
    {
      errloc = "seeking to end";
      goto err_no;
    }

  if ((fsize = ftell (f)) < 0)
    {
      errloc = "filesize determination";
      goto err_no;
    }

  if (fseek (f, 0, SEEK_SET) < 0)
    {
      errloc = "filepos resetting";
      goto err_no;
    }

  if ((buf = malloc (fsize)) == NULL)
    {
      errloc = "CTF archive buffer allocation";
      goto err_no;
    }

  while (!feof (f) && fread (buf, fsize, 1, f) == 0)
    if (ferror (f))
      {
	errloc = "reading archive from temporary file";
	goto err_no;
      }

  free (arg.names);
  free (arg.files);
  return buf;

 err_no:
  err = errno;
 err:
  free (buf);
  if (f)
    fclose (f);
  free (arg.names);
  free (arg.files);
  ctf_dprintf ("Cannot write archive in link: %s failure: %s\n", errloc,
	       ctf_errmsg (err));
  ctf_set_errno (fp, err);
  return NULL;
}

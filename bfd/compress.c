/* Compressed section support (intended for debug sections).
   Copyright (C) 2008-2021 Free Software Foundation, Inc.

   This file is part of BFD, the Binary File Descriptor library.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston,
   MA 02110-1301, USA.  */

#include "sysdep.h"
#include <zlib.h>
#include "bfd.h"
#include "libbfd.h"
#include "safe-ctype.h"

#define MAX_COMPRESSION_HEADER_SIZE 24

static bool
decompress_contents (bfd_byte *compressed_buffer,
		     bfd_size_type compressed_size,
		     bfd_byte *uncompressed_buffer,
		     bfd_size_type uncompressed_size)
{
  z_stream strm;
  int rc;

  /* It is possible the section consists of several compressed
     buffers concatenated together, so we uncompress in a loop.  */
  /* PR 18313: The state field in the z_stream structure is supposed
     to be invisible to the user (ie us), but some compilers will
     still complain about it being used without initialisation.  So
     we first zero the entire z_stream structure and then set the fields
     that we need.  */
  memset (& strm, 0, sizeof strm);
  strm.avail_in = compressed_size;
  strm.next_in = (Bytef*) compressed_buffer;
  strm.avail_out = uncompressed_size;

  BFD_ASSERT (Z_OK == 0);
  rc = inflateInit (&strm);
  while (strm.avail_in > 0 && strm.avail_out > 0)
    {
      if (rc != Z_OK)
	break;
      strm.next_out = ((Bytef*) uncompressed_buffer
		       + (uncompressed_size - strm.avail_out));
      rc = inflate (&strm, Z_FINISH);
      if (rc != Z_STREAM_END)
	break;
      rc = inflateReset (&strm);
    }
  return inflateEnd (&strm) == Z_OK && rc == Z_OK && strm.avail_out == 0;
}

/* Compress data of the size specified in @var{uncompressed_size}
   and pointed to by @var{uncompressed_buffer} using zlib and store
   as the contents field.  This function assumes the contents
   field was allocated using bfd_malloc() or equivalent.

   Return the uncompressed size if the full section contents is
   compressed successfully.  Otherwise return 0.  */

static bfd_size_type
bfd_compress_section_contents (bfd *abfd, sec_ptr sec,
			       bfd_byte *uncompressed_buffer,
			       bfd_size_type uncompressed_size)
{
  uLong compressed_size;
  bfd_byte *buffer;
  bfd_size_type buffer_size;
  bool decompress;
  int zlib_size = 0;
  int orig_compression_header_size;
  bfd_size_type orig_uncompressed_size;
  unsigned int orig_uncompressed_alignment_pow;
  int header_size = bfd_get_compression_header_size (abfd, NULL);
  bool compressed
    = bfd_is_section_compressed_with_header (abfd, sec,
					     &orig_compression_header_size,
					     &orig_uncompressed_size,
					     &orig_uncompressed_alignment_pow);

  /* Either ELF compression header or the 12-byte, "ZLIB" + 8-byte size,
     overhead in .zdebug* section.  */
  if (!header_size)
     header_size = 12;

  if (compressed)
    {
      /* We shouldn't decompress unsupported compressed section.  */
      if (orig_compression_header_size < 0)
	abort ();

      /* Different compression schemes.  Just move the compressed section
	 contents to the right position. */
      if (orig_compression_header_size == 0)
	{
	  /* Convert it from .zdebug* section.  Get the uncompressed
	     size first.  We need to subtract the 12-byte overhead in
	     .zdebug* section.  Set orig_compression_header_size to
	     the 12-bye overhead.  */
	  orig_compression_header_size = 12;
	  zlib_size = uncompressed_size - 12;
	}
      else
	{
	  /* Convert it to .zdebug* section.  */
	  zlib_size = uncompressed_size - orig_compression_header_size;
	}

      /* Add the header size.  */
      compressed_size = zlib_size + header_size;
    }
  else
    compressed_size = compressBound (uncompressed_size) + header_size;

  /* Uncompress if it leads to smaller size.  */
  if (compressed && compressed_size > orig_uncompressed_size)
    {
      decompress = true;
      buffer_size = orig_uncompressed_size;
    }
  else
    {
      decompress = false;
      buffer_size = compressed_size;
    }
  buffer = (bfd_byte *) bfd_alloc (abfd, buffer_size);
  if (buffer == NULL)
    return 0;

  if (compressed)
    {
      sec->size = orig_uncompressed_size;
      if (decompress)
	{
	  if (!decompress_contents (uncompressed_buffer
				    + orig_compression_header_size,
				    zlib_size, buffer, buffer_size))
	    {
	      bfd_set_error (bfd_error_bad_value);
	      bfd_release (abfd, buffer);
	      return 0;
	    }
	  free (uncompressed_buffer);
	  bfd_set_section_alignment (sec, orig_uncompressed_alignment_pow);

	  sec->contents = buffer;
	  sec->compress_status = COMPRESS_SECTION_DONE;
	  return orig_uncompressed_size;
	}
      else
	{
	  bfd_update_compression_header (abfd, buffer, sec);
	  memmove (buffer + header_size,
		   uncompressed_buffer + orig_compression_header_size,
		   zlib_size);
	}
    }
  else
    {
      if (compress ((Bytef*) buffer + header_size,
		    &compressed_size,
		    (const Bytef*) uncompressed_buffer,
		    uncompressed_size) != Z_OK)
	{
	  bfd_release (abfd, buffer);
	  bfd_set_error (bfd_error_bad_value);
	  return 0;
	}

      compressed_size += header_size;
      /* PR binutils/18087: If compression didn't make the section smaller,
	 just keep it uncompressed.  */
      if (compressed_size < uncompressed_size)
	bfd_update_compression_header (abfd, buffer, sec);
      else
	{
	  /* NOTE: There is a small memory leak here since
	     uncompressed_buffer is malloced and won't be freed.  */
	  bfd_release (abfd, buffer);
	  sec->contents = uncompressed_buffer;
	  sec->compress_status = COMPRESS_SECTION_NONE;
	  return uncompressed_size;
	}
    }

  free (uncompressed_buffer);
  sec->contents = buffer;
  sec->size = compressed_size;
  sec->compress_status = COMPRESS_SECTION_DONE;

  return uncompressed_size;
}

/*
FUNCTION
	bfd_get_full_section_contents

SYNOPSIS
	bool bfd_get_full_section_contents
	  (bfd *abfd, asection *section, bfd_byte **ptr);

DESCRIPTION
	Read all data from @var{section} in BFD @var{abfd}, decompress
	if needed, and store in @var{*ptr}.  If @var{*ptr} is NULL,
	return @var{*ptr} with memory malloc'd by this function.

	Return @code{TRUE} if the full section contents is retrieved
	successfully.  If the section has no contents then this function
	returns @code{TRUE} but @var{*ptr} is set to NULL.
*/

bool
bfd_get_full_section_contents (bfd *abfd, sec_ptr sec, bfd_byte **ptr)
{
  bfd_size_type sz;
  bfd_byte *p = *ptr;
  bool ret;
  bfd_size_type save_size;
  bfd_size_type save_rawsize;
  bfd_byte *compressed_buffer;
  unsigned int compression_header_size;

  if (abfd->direction != write_direction && sec->rawsize != 0)
    sz = sec->rawsize;
  else
    sz = sec->size;
  if (sz == 0)
    {
      *ptr = NULL;
      return true;
    }

  switch (sec->compress_status)
    {
    case COMPRESS_SECTION_NONE:
      if (p == NULL)
	{
	  ufile_ptr filesize = bfd_get_file_size (abfd);
	  if (filesize > 0
	      && filesize < sz
	      /* PR 24753: Linker created sections can be larger than
		 the file size, eg if they are being used to hold stubs.  */
	      && (bfd_section_flags (sec) & SEC_LINKER_CREATED) == 0
	      /* PR 24753: Sections which have no content should also be
		 excluded as they contain no size on disk.  */
	      && (bfd_section_flags (sec) & SEC_HAS_CONTENTS) != 0
	      /* The MMO file format supports its own special compression
		 technique, but it uses COMPRESS_SECTION_NONE when loading
		 a section's contents.  */
	      && bfd_get_flavour (abfd) != bfd_target_mmo_flavour)
	    {
	      /* PR 24708: Avoid attempts to allocate a ridiculous amount
		 of memory.  */
	      bfd_set_error (bfd_error_file_truncated);
	      _bfd_error_handler
		/* xgettext:c-format */
		(_("error: %pB(%pA) section size (%#" PRIx64 " bytes) is larger than file size (%#" PRIx64 " bytes)"),
		 abfd, sec, (uint64_t) sz, (uint64_t) filesize);
	      return false;
	    }
	  p = (bfd_byte *) bfd_malloc (sz);
	  if (p == NULL)
	    {
	      /* PR 20801: Provide a more helpful error message.  */
	      if (bfd_get_error () == bfd_error_no_memory)
		_bfd_error_handler
		  /* xgettext:c-format */
		  (_("error: %pB(%pA) is too large (%#" PRIx64 " bytes)"),
		  abfd, sec, (uint64_t) sz);
	      return false;
	    }
	}

      if (!bfd_get_section_contents (abfd, sec, p, 0, sz))
	{
	  if (*ptr != p)
	    free (p);
	  return false;
	}
      *ptr = p;
      return true;

    case DECOMPRESS_SECTION_SIZED:
      /* Read in the full compressed section contents.  */
      compressed_buffer = (bfd_byte *) bfd_malloc (sec->compressed_size);
      if (compressed_buffer == NULL)
	return false;
      save_rawsize = sec->rawsize;
      save_size = sec->size;
      /* Clear rawsize, set size to compressed size and set compress_status
	 to COMPRESS_SECTION_NONE.  If the compressed size is bigger than
	 the uncompressed size, bfd_get_section_contents will fail.  */
      sec->rawsize = 0;
      sec->size = sec->compressed_size;
      sec->compress_status = COMPRESS_SECTION_NONE;
      ret = bfd_get_section_contents (abfd, sec, compressed_buffer,
				      0, sec->compressed_size);
      /* Restore rawsize and size.  */
      sec->rawsize = save_rawsize;
      sec->size = save_size;
      sec->compress_status = DECOMPRESS_SECTION_SIZED;
      if (!ret)
	goto fail_compressed;

      if (p == NULL)
	p = (bfd_byte *) bfd_malloc (sz);
      if (p == NULL)
	goto fail_compressed;

      compression_header_size = bfd_get_compression_header_size (abfd, sec);
      if (compression_header_size == 0)
	/* Set header size to the zlib header size if it is a
	   SHF_COMPRESSED section.  */
	compression_header_size = 12;
      if (!decompress_contents (compressed_buffer + compression_header_size,
				sec->compressed_size - compression_header_size, p, sz))
	{
	  bfd_set_error (bfd_error_bad_value);
	  if (p != *ptr)
	    free (p);
	fail_compressed:
	  free (compressed_buffer);
	  return false;
	}

      free (compressed_buffer);
      *ptr = p;
      return true;

    case COMPRESS_SECTION_DONE:
      if (sec->contents == NULL)
	return false;
      if (p == NULL)
	{
	  p = (bfd_byte *) bfd_malloc (sz);
	  if (p == NULL)
	    return false;
	  *ptr = p;
	}
      /* PR 17512; file: 5bc29788.  */
      if (p != sec->contents)
	memcpy (p, sec->contents, sz);
      return true;

    default:
      abort ();
    }
}

/*
FUNCTION
	bfd_cache_section_contents

SYNOPSIS
	void bfd_cache_section_contents
	  (asection *sec, void *contents);

DESCRIPTION
	Stash @var(contents) so any following reads of @var(sec) do
	not need to decompress again.
*/

void
bfd_cache_section_contents (asection *sec, void *contents)
{
  if (sec->compress_status == DECOMPRESS_SECTION_SIZED)
    sec->compress_status = COMPRESS_SECTION_DONE;
  sec->contents = contents;
  sec->flags |= SEC_IN_MEMORY;
}

/*
FUNCTION
	bfd_is_section_compressed_with_header

SYNOPSIS
	bool bfd_is_section_compressed_with_header
	  (bfd *abfd, asection *section,
	  int *compression_header_size_p,
	  bfd_size_type *uncompressed_size_p,
	  unsigned int *uncompressed_alignment_power_p);

DESCRIPTION
	Return @code{TRUE} if @var{section} is compressed.  Compression
	header size is returned in @var{compression_header_size_p},
	uncompressed size is returned in @var{uncompressed_size_p}
	and the uncompressed data alignement power is returned in
	@var{uncompressed_align_pow_p}.  If compression is
	unsupported, compression header size is returned with -1
	and uncompressed size is returned with 0.
*/

bool
bfd_is_section_compressed_with_header (bfd *abfd, sec_ptr sec,
				       int *compression_header_size_p,
				       bfd_size_type *uncompressed_size_p,
				       unsigned int *uncompressed_align_pow_p)
{
  bfd_byte header[MAX_COMPRESSION_HEADER_SIZE];
  int compression_header_size;
  int header_size;
  unsigned int saved = sec->compress_status;
  bool compressed;

  *uncompressed_align_pow_p = 0;

  compression_header_size = bfd_get_compression_header_size (abfd, sec);
  if (compression_header_size > MAX_COMPRESSION_HEADER_SIZE)
    abort ();
  header_size = compression_header_size ? compression_header_size : 12;

  /* Don't decompress the section.  */
  sec->compress_status = COMPRESS_SECTION_NONE;

  /* Read the header.  */
  if (bfd_get_section_contents (abfd, sec, header, 0, header_size))
    {
      if (compression_header_size == 0)
	/* In this case, it should be "ZLIB" followed by the uncompressed
	   section size, 8 bytes in big-endian order.  */
	compressed = startswith ((char*) header , "ZLIB");
      else
	compressed = true;
    }
  else
    compressed = false;

  *uncompressed_size_p = sec->size;
  if (compressed)
    {
      if (compression_header_size != 0)
	{
	  if (!bfd_check_compression_header (abfd, header, sec,
					     uncompressed_size_p,
					     uncompressed_align_pow_p))
	    compression_header_size = -1;
	}
      /* Check for the pathalogical case of a debug string section that
	 contains the string ZLIB.... as the first entry.  We assume that
	 no uncompressed .debug_str section would ever be big enough to
	 have the first byte of its (big-endian) size be non-zero.  */
      else if (strcmp (sec->name, ".debug_str") == 0
	       && ISPRINT (header[4]))
	compressed = false;
      else
	*uncompressed_size_p = bfd_getb64 (header + 4);
    }

  /* Restore compress_status.  */
  sec->compress_status = saved;
  *compression_header_size_p = compression_header_size;
  return compressed;
}

/*
FUNCTION
	bfd_is_section_compressed

SYNOPSIS
	bool bfd_is_section_compressed
	  (bfd *abfd, asection *section);

DESCRIPTION
	Return @code{TRUE} if @var{section} is compressed.
*/

bool
bfd_is_section_compressed (bfd *abfd, sec_ptr sec)
{
  int compression_header_size;
  bfd_size_type uncompressed_size;
  unsigned int uncompressed_align_power;
  return (bfd_is_section_compressed_with_header (abfd, sec,
						 &compression_header_size,
						 &uncompressed_size,
						 &uncompressed_align_power)
	  && compression_header_size >= 0
	  && uncompressed_size > 0);
}

/*
FUNCTION
	bfd_init_section_decompress_status

SYNOPSIS
	bool bfd_init_section_decompress_status
	  (bfd *abfd, asection *section);

DESCRIPTION
	Record compressed section size, update section size with
	decompressed size and set compress_status to
	DECOMPRESS_SECTION_SIZED.

	Return @code{FALSE} if the section is not a valid compressed
	section.  Otherwise, return @code{TRUE}.
*/

bool
bfd_init_section_decompress_status (bfd *abfd, sec_ptr sec)
{
  bfd_byte header[MAX_COMPRESSION_HEADER_SIZE];
  int compression_header_size;
  int header_size;
  bfd_size_type uncompressed_size;
  unsigned int uncompressed_alignment_power = 0;

  compression_header_size = bfd_get_compression_header_size (abfd, sec);
  if (compression_header_size > MAX_COMPRESSION_HEADER_SIZE)
    abort ();
  header_size = compression_header_size ? compression_header_size : 12;

  /* Read the header.  */
  if (sec->rawsize != 0
      || sec->contents != NULL
      || sec->compress_status != COMPRESS_SECTION_NONE
      || !bfd_get_section_contents (abfd, sec, header, 0, header_size))
    {
      bfd_set_error (bfd_error_invalid_operation);
      return false;
    }

  if (compression_header_size == 0)
    {
      /* In this case, it should be "ZLIB" followed by the uncompressed
	 section size, 8 bytes in big-endian order.  */
      if (! startswith ((char*) header, "ZLIB"))
	{
	  bfd_set_error (bfd_error_wrong_format);
	  return false;
	}
      uncompressed_size = bfd_getb64 (header + 4);
    }
  else if (!bfd_check_compression_header (abfd, header, sec,
					  &uncompressed_size,
					  &uncompressed_alignment_power))
    {
      bfd_set_error (bfd_error_wrong_format);
      return false;
    }

  sec->compressed_size = sec->size;
  sec->size = uncompressed_size;
  bfd_set_section_alignment (sec, uncompressed_alignment_power);
  sec->compress_status = DECOMPRESS_SECTION_SIZED;

  return true;
}

/*
FUNCTION
	bfd_init_section_compress_status

SYNOPSIS
	bool bfd_init_section_compress_status
	  (bfd *abfd, asection *section);

DESCRIPTION
	If open for read, compress section, update section size with
	compressed size and set compress_status to COMPRESS_SECTION_DONE.

	Return @code{FALSE} if the section is not a valid compressed
	section.  Otherwise, return @code{TRUE}.
*/

bool
bfd_init_section_compress_status (bfd *abfd, sec_ptr sec)
{
  bfd_size_type uncompressed_size;
  bfd_byte *uncompressed_buffer;

  /* Error if not opened for read.  */
  if (abfd->direction != read_direction
      || sec->size == 0
      || sec->rawsize != 0
      || sec->contents != NULL
      || sec->compress_status != COMPRESS_SECTION_NONE)
    {
      bfd_set_error (bfd_error_invalid_operation);
      return false;
    }

  /* Read in the full section contents and compress it.  */
  uncompressed_size = sec->size;
  uncompressed_buffer = (bfd_byte *) bfd_malloc (uncompressed_size);
  /* PR 21431 */
  if (uncompressed_buffer == NULL)
    return false;

  if (!bfd_get_section_contents (abfd, sec, uncompressed_buffer,
				 0, uncompressed_size))
    return false;

  uncompressed_size = bfd_compress_section_contents (abfd, sec,
						     uncompressed_buffer,
						     uncompressed_size);
  return uncompressed_size != 0;
}

/*
FUNCTION
	bfd_compress_section

SYNOPSIS
	bool bfd_compress_section
	  (bfd *abfd, asection *section, bfd_byte *uncompressed_buffer);

DESCRIPTION
	If open for write, compress section, update section size with
	compressed size and set compress_status to COMPRESS_SECTION_DONE.

	Return @code{FALSE} if compression fail.  Otherwise, return
	@code{TRUE}.
*/

bool
bfd_compress_section (bfd *abfd, sec_ptr sec, bfd_byte *uncompressed_buffer)
{
  bfd_size_type uncompressed_size = sec->size;

  /* Error if not opened for write.  */
  if (abfd->direction != write_direction
      || uncompressed_size == 0
      || uncompressed_buffer == NULL
      || sec->contents != NULL
      || sec->compressed_size != 0
      || sec->compress_status != COMPRESS_SECTION_NONE)
    {
      bfd_set_error (bfd_error_invalid_operation);
      return false;
    }

  /* Compress it.  */
  return bfd_compress_section_contents (abfd, sec, uncompressed_buffer,
					uncompressed_size) != 0;
}

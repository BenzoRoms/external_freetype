/***************************************************************************/
/*                                                                         */
/*  cffgload.c                                                             */
/*                                                                         */
/*    OpenType Glyph Loader (body).                                        */
/*                                                                         */
/*  Copyright 1996-2017 by                                                 */
/*  David Turner, Robert Wilhelm, and Werner Lemberg.                      */
/*                                                                         */
/*  This file is part of the FreeType project, and may only be used,       */
/*  modified, and distributed under the terms of the FreeType project      */
/*  license, LICENSE.TXT.  By continuing to use, modify, or distribute     */
/*  this file you indicate that you have read the license and              */
/*  understand and accept it fully.                                        */
/*                                                                         */
/***************************************************************************/


#include <ft2build.h>
#include FT_INTERNAL_DEBUG_H
#include FT_INTERNAL_STREAM_H
#include FT_INTERNAL_SFNT_H
#include FT_INTERNAL_CALC_H
#include FT_OUTLINE_H
#include FT_CFF_DRIVER_H

#include "cffobjs.h"
#include "cffload.h"
#include "cffgload.h"

#include "cfferrs.h"


  /*************************************************************************/
  /*                                                                       */
  /* The macro FT_COMPONENT is used in trace mode.  It is an implicit      */
  /* parameter of the FT_TRACE() and FT_ERROR() macros, used to print/log  */
  /* messages during execution.                                            */
  /*                                                                       */
#undef  FT_COMPONENT
#define FT_COMPONENT  trace_cffgload




  /*************************************************************************/
  /*************************************************************************/
  /*************************************************************************/
  /**********                                                      *********/
  /**********                                                      *********/
  /**********             GENERIC CHARSTRING PARSING               *********/
  /**********                                                      *********/
  /**********                                                      *********/
  /*************************************************************************/
  /*************************************************************************/
  /*************************************************************************/


  /*************************************************************************/
  /*                                                                       */
  /* <Function>                                                            */
  /*    cff_builder_init                                                   */
  /*                                                                       */
  /* <Description>                                                         */
  /*    Initializes a given glyph builder.                                 */
  /*                                                                       */
  /* <InOut>                                                               */
  /*    builder :: A pointer to the glyph builder to initialize.           */
  /*                                                                       */
  /* <Input>                                                               */
  /*    face    :: The current face object.                                */
  /*                                                                       */
  /*    size    :: The current size object.                                */
  /*                                                                       */
  /*    glyph   :: The current glyph object.                               */
  /*                                                                       */
  /*    hinting :: Whether hinting is active.                              */
  /*                                                                       */
  static void
  cff_builder_init( CFF_Builder*   builder,
                    TT_Face        face,
                    CFF_Size       size,
                    CFF_GlyphSlot  glyph,
                    FT_Bool        hinting )
  {
    builder->path_begun  = 0;
    builder->load_points = 1;

    builder->face   = face;
    builder->glyph  = glyph;
    builder->memory = face->root.memory;

    if ( glyph )
    {
      FT_GlyphLoader  loader = glyph->root.internal->loader;


      builder->loader  = loader;
      builder->base    = &loader->base.outline;
      builder->current = &loader->current.outline;
      FT_GlyphLoader_Rewind( loader );

      builder->hints_globals = NULL;
      builder->hints_funcs   = NULL;

      if ( hinting && size )
      {
        FT_Size       ftsize   = FT_SIZE( size );
        CFF_Internal  internal = (CFF_Internal)ftsize->internal->module_data;


        if ( internal )
        {
          builder->hints_globals = (void *)internal->topfont;
          builder->hints_funcs   = glyph->root.internal->glyph_hints;
        }
      }
    }

    builder->pos_x = 0;
    builder->pos_y = 0;

    builder->left_bearing.x = 0;
    builder->left_bearing.y = 0;
    builder->advance.x      = 0;
    builder->advance.y      = 0;
  }


  /*************************************************************************/
  /*                                                                       */
  /* <Function>                                                            */
  /*    cff_builder_done                                                   */
  /*                                                                       */
  /* <Description>                                                         */
  /*    Finalizes a given glyph builder.  Its contents can still be used   */
  /*    after the call, but the function saves important information       */
  /*    within the corresponding glyph slot.                               */
  /*                                                                       */
  /* <Input>                                                               */
  /*    builder :: A pointer to the glyph builder to finalize.             */
  /*                                                                       */
  static void
  cff_builder_done( CFF_Builder*  builder )
  {
    CFF_GlyphSlot  glyph = builder->glyph;


    if ( glyph )
      glyph->root.outline = *builder->base;
  }



  /* check that there is enough space for `count' more points */
  FT_LOCAL_DEF( FT_Error )
  cff_check_points( CFF_Builder*  builder,
                    FT_Int        count )
  {
    return FT_GLYPHLOADER_CHECK_POINTS( builder->loader, count, 0 );
  }


  /* add a new point, do not check space */
  FT_LOCAL_DEF( void )
  cff_builder_add_point( CFF_Builder*  builder,
                         FT_Pos        x,
                         FT_Pos        y,
                         FT_Byte       flag )
  {
    FT_Outline*  outline = builder->current;


    if ( builder->load_points )
    {
      FT_Vector*  point   = outline->points + outline->n_points;
      FT_Byte*    control = (FT_Byte*)outline->tags + outline->n_points;

#ifdef CFF_CONFIG_OPTION_OLD_ENGINE
      CFF_Driver  driver  = (CFF_Driver)FT_FACE_DRIVER( builder->face );


      if ( driver->hinting_engine == FT_CFF_HINTING_FREETYPE )
      {
        point->x = x >> 16;
        point->y = y >> 16;
      }
      else
#endif
      {
        /* cf2_decoder_parse_charstrings uses 16.16 coordinates */
        point->x = x >> 10;
        point->y = y >> 10;
      }
      *control = (FT_Byte)( flag ? FT_CURVE_TAG_ON : FT_CURVE_TAG_CUBIC );
    }

    outline->n_points++;
  }


  /* check space for a new on-curve point, then add it */
  FT_LOCAL_DEF( FT_Error )
  cff_builder_add_point1( CFF_Builder*  builder,
                          FT_Pos        x,
                          FT_Pos        y )
  {
    FT_Error  error;


    error = cff_check_points( builder, 1 );
    if ( !error )
      cff_builder_add_point( builder, x, y, 1 );

    return error;
  }


  /* check space for a new contour, then add it */
  static FT_Error
  cff_builder_add_contour( CFF_Builder*  builder )
  {
    FT_Outline*  outline = builder->current;
    FT_Error     error;


    if ( !builder->load_points )
    {
      outline->n_contours++;
      return FT_Err_Ok;
    }

    error = FT_GLYPHLOADER_CHECK_POINTS( builder->loader, 0, 1 );
    if ( !error )
    {
      if ( outline->n_contours > 0 )
        outline->contours[outline->n_contours - 1] =
          (short)( outline->n_points - 1 );

      outline->n_contours++;
    }

    return error;
  }


  /* if a path was begun, add its first on-curve point */
  FT_LOCAL_DEF( FT_Error )
  cff_builder_start_point( CFF_Builder*  builder,
                           FT_Pos        x,
                           FT_Pos        y )
  {
    FT_Error  error = FT_Err_Ok;


    /* test whether we are building a new contour */
    if ( !builder->path_begun )
    {
      builder->path_begun = 1;
      error = cff_builder_add_contour( builder );
      if ( !error )
        error = cff_builder_add_point1( builder, x, y );
    }

    return error;
  }


  /* close the current contour */
  FT_LOCAL_DEF( void )
  cff_builder_close_contour( CFF_Builder*  builder )
  {
    FT_Outline*  outline = builder->current;
    FT_Int       first;


    if ( !outline )
      return;

    first = outline->n_contours <= 1
            ? 0 : outline->contours[outline->n_contours - 2] + 1;

    /* We must not include the last point in the path if it */
    /* is located on the first point.                       */
    if ( outline->n_points > 1 )
    {
      FT_Vector*  p1      = outline->points + first;
      FT_Vector*  p2      = outline->points + outline->n_points - 1;
      FT_Byte*    control = (FT_Byte*)outline->tags + outline->n_points - 1;


      /* `delete' last point only if it coincides with the first    */
      /* point and if it is not a control point (which can happen). */
      if ( p1->x == p2->x && p1->y == p2->y )
        if ( *control == FT_CURVE_TAG_ON )
          outline->n_points--;
    }

    if ( outline->n_contours > 0 )
    {
      /* Don't add contours only consisting of one point, i.e., */
      /* check whether begin point and last point are the same. */
      if ( first == outline->n_points - 1 )
      {
        outline->n_contours--;
        outline->n_points--;
      }
      else
        outline->contours[outline->n_contours - 1] =
          (short)( outline->n_points - 1 );
    }
  }




  FT_LOCAL_DEF( FT_Error )
  cff_get_glyph_data( TT_Face    face,
                      FT_UInt    glyph_index,
                      FT_Byte**  pointer,
                      FT_ULong*  length )
  {
#ifdef FT_CONFIG_OPTION_INCREMENTAL
    /* For incremental fonts get the character data using the */
    /* callback function.                                     */
    if ( face->root.internal->incremental_interface )
    {
      FT_Data   data;
      FT_Error  error =
                  face->root.internal->incremental_interface->funcs->get_glyph_data(
                    face->root.internal->incremental_interface->object,
                    glyph_index, &data );


      *pointer = (FT_Byte*)data.pointer;
      *length  = (FT_ULong)data.length;

      return error;
    }
    else
#endif /* FT_CONFIG_OPTION_INCREMENTAL */

    {
      CFF_Font  cff = (CFF_Font)(face->extra.data);


      return cff_index_access_element( &cff->charstrings_index, glyph_index,
                                       pointer, length );
    }
  }


  FT_LOCAL_DEF( void )
  cff_free_glyph_data( TT_Face    face,
                       FT_Byte**  pointer,
                       FT_ULong   length )
  {
#ifndef FT_CONFIG_OPTION_INCREMENTAL
    FT_UNUSED( length );
#endif

#ifdef FT_CONFIG_OPTION_INCREMENTAL
    /* For incremental fonts get the character data using the */
    /* callback function.                                     */
    if ( face->root.internal->incremental_interface )
    {
      FT_Data  data;


      data.pointer = *pointer;
      data.length  = (FT_Int)length;

      face->root.internal->incremental_interface->funcs->free_glyph_data(
        face->root.internal->incremental_interface->object, &data );
    }
    else
#endif /* FT_CONFIG_OPTION_INCREMENTAL */

    {
      CFF_Font  cff = (CFF_Font)(face->extra.data);


      cff_index_forget_element( &cff->charstrings_index, pointer );
    }
  }





  /*************************************************************************/
  /*************************************************************************/
  /*************************************************************************/
  /**********                                                      *********/
  /**********                                                      *********/
  /**********            COMPUTE THE MAXIMUM ADVANCE WIDTH         *********/
  /**********                                                      *********/
  /**********    The following code is in charge of computing      *********/
  /**********    the maximum advance width of the font.  It        *********/
  /**********    quickly processes each glyph charstring to        *********/
  /**********    extract the value from either a `sbw' or `seac'   *********/
  /**********    operator.                                         *********/
  /**********                                                      *********/
  /*************************************************************************/
  /*************************************************************************/
  /*************************************************************************/


#if 0 /* unused until we support pure CFF fonts */


  FT_LOCAL_DEF( FT_Error )
  cff_compute_max_advance( TT_Face  face,
                           FT_Int*  max_advance )
  {
    FT_Error     error = FT_Err_Ok;
    CFF_Decoder  decoder;
    FT_Int       glyph_index;
    CFF_Font     cff = (CFF_Font)face->other;


    *max_advance = 0;

    /* Initialize load decoder */
    cff_decoder_init( &decoder, face, 0, 0, 0, 0 );

    decoder.builder.metrics_only = 1;
    decoder.builder.load_points  = 0;

    /* For each glyph, parse the glyph charstring and extract */
    /* the advance width.                                     */
    for ( glyph_index = 0; glyph_index < face->root.num_glyphs;
          glyph_index++ )
    {
      FT_Byte*  charstring;
      FT_ULong  charstring_len;


      /* now get load the unscaled outline */
      error = cff_get_glyph_data( face, glyph_index,
                                  &charstring, &charstring_len );
      if ( !error )
      {
        error = cff_decoder_prepare( &decoder, size, glyph_index );
        if ( !error )
          error = cff_decoder_parse_charstrings( &decoder,
                                                 charstring,
                                                 charstring_len,
                                                 0 );

        cff_free_glyph_data( face, &charstring, &charstring_len );
      }

      /* ignore the error if one has occurred -- skip to next glyph */
      error = FT_Err_Ok;
    }

    *max_advance = decoder.builder.advance.x;

    return FT_Err_Ok;
  }


#endif /* 0 */


  FT_LOCAL_DEF( FT_Error )
  cff_slot_load( CFF_GlyphSlot  glyph,
                 CFF_Size       size,
                 FT_UInt        glyph_index,
                 FT_Int32       load_flags )
  {
    FT_Error     error;
    CFF_Decoder  decoder;
    TT_Face      face = (TT_Face)glyph->root.face;
    FT_Bool      hinting, scaled, force_scaling;
    CFF_Font     cff  = (CFF_Font)face->extra.data;

    FT_Matrix    font_matrix;
    FT_Vector    font_offset;


    force_scaling = FALSE;

    /* in a CID-keyed font, consider `glyph_index' as a CID and map */
    /* it immediately to the real glyph_index -- if it isn't a      */
    /* subsetted font, glyph_indices and CIDs are identical, though */
    if ( cff->top_font.font_dict.cid_registry != 0xFFFFU &&
         cff->charset.cids                               )
    {
      /* don't handle CID 0 (.notdef) which is directly mapped to GID 0 */
      if ( glyph_index != 0 )
      {
        glyph_index = cff_charset_cid_to_gindex( &cff->charset,
                                                 glyph_index );
        if ( glyph_index == 0 )
          return FT_THROW( Invalid_Argument );
      }
    }
    else if ( glyph_index >= cff->num_glyphs )
      return FT_THROW( Invalid_Argument );

    if ( load_flags & FT_LOAD_NO_RECURSE )
      load_flags |= FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING;

    glyph->x_scale = 0x10000L;
    glyph->y_scale = 0x10000L;
    if ( size )
    {
      glyph->x_scale = size->root.metrics.x_scale;
      glyph->y_scale = size->root.metrics.y_scale;
    }

#ifdef TT_CONFIG_OPTION_EMBEDDED_BITMAPS

    /* try to load embedded bitmap if any              */
    /*                                                 */
    /* XXX: The convention should be emphasized in     */
    /*      the documents because it can be confusing. */
    if ( size )
    {
      CFF_Face      cff_face = (CFF_Face)size->root.face;
      SFNT_Service  sfnt     = (SFNT_Service)cff_face->sfnt;
      FT_Stream     stream   = cff_face->root.stream;


      if ( size->strike_index != 0xFFFFFFFFUL      &&
           sfnt->load_eblc                         &&
           ( load_flags & FT_LOAD_NO_BITMAP ) == 0 )
      {
        TT_SBit_MetricsRec  metrics;


        error = sfnt->load_sbit_image( face,
                                       size->strike_index,
                                       glyph_index,
                                       (FT_UInt)load_flags,
                                       stream,
                                       &glyph->root.bitmap,
                                       &metrics );

        if ( !error )
        {
          FT_Bool    has_vertical_info;
          FT_UShort  advance;
          FT_Short   dummy;


          glyph->root.outline.n_points   = 0;
          glyph->root.outline.n_contours = 0;

          glyph->root.metrics.width  = (FT_Pos)metrics.width  << 6;
          glyph->root.metrics.height = (FT_Pos)metrics.height << 6;

          glyph->root.metrics.horiBearingX = (FT_Pos)metrics.horiBearingX << 6;
          glyph->root.metrics.horiBearingY = (FT_Pos)metrics.horiBearingY << 6;
          glyph->root.metrics.horiAdvance  = (FT_Pos)metrics.horiAdvance  << 6;

          glyph->root.metrics.vertBearingX = (FT_Pos)metrics.vertBearingX << 6;
          glyph->root.metrics.vertBearingY = (FT_Pos)metrics.vertBearingY << 6;
          glyph->root.metrics.vertAdvance  = (FT_Pos)metrics.vertAdvance  << 6;

          glyph->root.format = FT_GLYPH_FORMAT_BITMAP;

          if ( load_flags & FT_LOAD_VERTICAL_LAYOUT )
          {
            glyph->root.bitmap_left = metrics.vertBearingX;
            glyph->root.bitmap_top  = metrics.vertBearingY;
          }
          else
          {
            glyph->root.bitmap_left = metrics.horiBearingX;
            glyph->root.bitmap_top  = metrics.horiBearingY;
          }

          /* compute linear advance widths */

          (void)( (SFNT_Service)face->sfnt )->get_metrics( face, 0,
                                                           glyph_index,
                                                           &dummy,
                                                           &advance );
          glyph->root.linearHoriAdvance = advance;

          has_vertical_info = FT_BOOL(
                                face->vertical_info                   &&
                                face->vertical.number_Of_VMetrics > 0 );

          /* get the vertical metrics from the vmtx table if we have one */
          if ( has_vertical_info )
          {
            (void)( (SFNT_Service)face->sfnt )->get_metrics( face, 1,
                                                             glyph_index,
                                                             &dummy,
                                                             &advance );
            glyph->root.linearVertAdvance = advance;
          }
          else
          {
            /* make up vertical ones */
            if ( face->os2.version != 0xFFFFU )
              glyph->root.linearVertAdvance = (FT_Pos)
                ( face->os2.sTypoAscender - face->os2.sTypoDescender );
            else
              glyph->root.linearVertAdvance = (FT_Pos)
                ( face->horizontal.Ascender - face->horizontal.Descender );
          }

          return error;
        }
      }
    }

#endif /* TT_CONFIG_OPTION_EMBEDDED_BITMAPS */

    /* return immediately if we only want the embedded bitmaps */
    if ( load_flags & FT_LOAD_SBITS_ONLY )
      return FT_THROW( Invalid_Argument );

    /* if we have a CID subfont, use its matrix (which has already */
    /* been multiplied with the root matrix)                       */

    /* this scaling is only relevant if the PS hinter isn't active */
    if ( cff->num_subfonts )
    {
      FT_Long  top_upm, sub_upm;
      FT_Byte  fd_index = cff_fd_select_get( &cff->fd_select,
                                             glyph_index );


      if ( fd_index >= cff->num_subfonts )
        fd_index = (FT_Byte)( cff->num_subfonts - 1 );

      top_upm = (FT_Long)cff->top_font.font_dict.units_per_em;
      sub_upm = (FT_Long)cff->subfonts[fd_index]->font_dict.units_per_em;


      font_matrix = cff->subfonts[fd_index]->font_dict.font_matrix;
      font_offset = cff->subfonts[fd_index]->font_dict.font_offset;

      if ( top_upm != sub_upm )
      {
        glyph->x_scale = FT_MulDiv( glyph->x_scale, top_upm, sub_upm );
        glyph->y_scale = FT_MulDiv( glyph->y_scale, top_upm, sub_upm );

        force_scaling = TRUE;
      }
    }
    else
    {
      font_matrix = cff->top_font.font_dict.font_matrix;
      font_offset = cff->top_font.font_dict.font_offset;
    }

    glyph->root.outline.n_points   = 0;
    glyph->root.outline.n_contours = 0;

    /* top-level code ensures that FT_LOAD_NO_HINTING is set */
    /* if FT_LOAD_NO_SCALE is active                         */
    hinting = FT_BOOL( ( load_flags & FT_LOAD_NO_HINTING ) == 0 );
    scaled  = FT_BOOL( ( load_flags & FT_LOAD_NO_SCALE   ) == 0 );

    glyph->hint        = hinting;
    glyph->scaled      = scaled;
    glyph->root.format = FT_GLYPH_FORMAT_OUTLINE;  /* by default */

    {
#ifdef CFF_CONFIG_OPTION_OLD_ENGINE
      CFF_Driver  driver = (CFF_Driver)FT_FACE_DRIVER( face );
#endif


      FT_Byte*  charstring;
      FT_ULong  charstring_len;


      cff_decoder_init( &decoder, face, size, glyph, hinting,
                        FT_LOAD_TARGET_MODE( load_flags ) );

      /* this is for pure CFFs */
      if ( load_flags & FT_LOAD_ADVANCE_ONLY )
        decoder.width_only = TRUE;

      decoder.builder.no_recurse =
        (FT_Bool)( load_flags & FT_LOAD_NO_RECURSE );

      /* now load the unscaled outline */
      error = cff_get_glyph_data( face, glyph_index,
                                  &charstring, &charstring_len );
      if ( error )
        goto Glyph_Build_Finished;

      error = cff_decoder_prepare( &decoder, size, glyph_index );
      if ( error )
        goto Glyph_Build_Finished;

#ifdef CFF_CONFIG_OPTION_OLD_ENGINE
      /* choose which CFF renderer to use */
      if ( driver->hinting_engine == FT_CFF_HINTING_FREETYPE )
        error = cff_decoder_parse_charstrings( &decoder,
                                               charstring,
                                               charstring_len,
                                               0 );
      else
#endif
      {
        error = cf2_decoder_parse_charstrings( &decoder,
                                               charstring,
                                               charstring_len );

        /* Adobe's engine uses 16.16 numbers everywhere;              */
        /* as a consequence, glyphs larger than 2000ppem get rejected */
        if ( FT_ERR_EQ( error, Glyph_Too_Big ) )
        {
          /* this time, we retry unhinted and scale up the glyph later on */
          /* (the engine uses and sets the hardcoded value 0x10000 / 64 = */
          /* 0x400 for both `x_scale' and `y_scale' in this case)         */
          hinting       = FALSE;
          force_scaling = TRUE;
          glyph->hint   = hinting;

          error = cf2_decoder_parse_charstrings( &decoder,
                                                 charstring,
                                                 charstring_len );
        }
      }

      cff_free_glyph_data( face, &charstring, charstring_len );

      if ( error )
        goto Glyph_Build_Finished;

#ifdef FT_CONFIG_OPTION_INCREMENTAL
      /* Control data and length may not be available for incremental */
      /* fonts.                                                       */
      if ( face->root.internal->incremental_interface )
      {
        glyph->root.control_data = NULL;
        glyph->root.control_len = 0;
      }
      else
#endif /* FT_CONFIG_OPTION_INCREMENTAL */

      /* We set control_data and control_len if charstrings is loaded. */
      /* See how charstring loads at cff_index_access_element() in     */
      /* cffload.c.                                                    */
      {
        CFF_Index  csindex = &cff->charstrings_index;


        if ( csindex->offsets )
        {
          glyph->root.control_data = csindex->bytes +
                                     csindex->offsets[glyph_index] - 1;
          glyph->root.control_len  = (FT_Long)charstring_len;
        }
      }

  Glyph_Build_Finished:
      /* save new glyph tables, if no error */
      if ( !error )
        cff_builder_done( &decoder.builder );
      /* XXX: anything to do for broken glyph entry? */
    }

#ifdef FT_CONFIG_OPTION_INCREMENTAL

    /* Incremental fonts can optionally override the metrics. */
    if ( !error                                                               &&
         face->root.internal->incremental_interface                           &&
         face->root.internal->incremental_interface->funcs->get_glyph_metrics )
    {
      FT_Incremental_MetricsRec  metrics;


      metrics.bearing_x = decoder.builder.left_bearing.x;
      metrics.bearing_y = 0;
      metrics.advance   = decoder.builder.advance.x;
      metrics.advance_v = decoder.builder.advance.y;

      error = face->root.internal->incremental_interface->funcs->get_glyph_metrics(
                face->root.internal->incremental_interface->object,
                glyph_index, FALSE, &metrics );

      decoder.builder.left_bearing.x = metrics.bearing_x;
      decoder.builder.advance.x      = metrics.advance;
      decoder.builder.advance.y      = metrics.advance_v;
    }

#endif /* FT_CONFIG_OPTION_INCREMENTAL */

    if ( !error )
    {
      /* Now, set the metrics -- this is rather simple, as   */
      /* the left side bearing is the xMin, and the top side */
      /* bearing the yMax.                                   */

      /* For composite glyphs, return only left side bearing and */
      /* advance width.                                          */
      if ( load_flags & FT_LOAD_NO_RECURSE )
      {
        FT_Slot_Internal  internal = glyph->root.internal;


        glyph->root.metrics.horiBearingX = decoder.builder.left_bearing.x;
        glyph->root.metrics.horiAdvance  = decoder.glyph_width;
        internal->glyph_matrix           = font_matrix;
        internal->glyph_delta            = font_offset;
        internal->glyph_transformed      = 1;
      }
      else
      {
        FT_BBox            cbox;
        FT_Glyph_Metrics*  metrics = &glyph->root.metrics;
        FT_Bool            has_vertical_info;


        if ( face->horizontal.number_Of_HMetrics )
        {
          FT_Short   horiBearingX = 0;
          FT_UShort  horiAdvance  = 0;


          ( (SFNT_Service)face->sfnt )->get_metrics( face, 0,
                                                     glyph_index,
                                                     &horiBearingX,
                                                     &horiAdvance );
          metrics->horiAdvance          = horiAdvance;
          metrics->horiBearingX         = horiBearingX;
          glyph->root.linearHoriAdvance = horiAdvance;
        }
        else
        {
          /* copy the _unscaled_ advance width */
          metrics->horiAdvance          = decoder.glyph_width;
          glyph->root.linearHoriAdvance = decoder.glyph_width;
        }

        glyph->root.internal->glyph_transformed = 0;

        has_vertical_info = FT_BOOL( face->vertical_info                   &&
                                     face->vertical.number_Of_VMetrics > 0 );

        /* get the vertical metrics from the vmtx table if we have one */
        if ( has_vertical_info )
        {
          FT_Short   vertBearingY = 0;
          FT_UShort  vertAdvance  = 0;


          ( (SFNT_Service)face->sfnt )->get_metrics( face, 1,
                                                     glyph_index,
                                                     &vertBearingY,
                                                     &vertAdvance );
          metrics->vertBearingY = vertBearingY;
          metrics->vertAdvance  = vertAdvance;
        }
        else
        {
          /* make up vertical ones */
          if ( face->os2.version != 0xFFFFU )
            metrics->vertAdvance = (FT_Pos)( face->os2.sTypoAscender -
                                             face->os2.sTypoDescender );
          else
            metrics->vertAdvance = (FT_Pos)( face->horizontal.Ascender -
                                             face->horizontal.Descender );
        }

        glyph->root.linearVertAdvance = metrics->vertAdvance;

        glyph->root.format = FT_GLYPH_FORMAT_OUTLINE;

        glyph->root.outline.flags = 0;
        if ( size && size->root.metrics.y_ppem < 24 )
          glyph->root.outline.flags |= FT_OUTLINE_HIGH_PRECISION;

        glyph->root.outline.flags |= FT_OUTLINE_REVERSE_FILL;

        /* apply the font matrix, if any */
        if ( font_matrix.xx != 0x10000L || font_matrix.yy != 0x10000L ||
             font_matrix.xy != 0        || font_matrix.yx != 0        )
        {
          FT_Outline_Transform( &glyph->root.outline, &font_matrix );

          metrics->horiAdvance = FT_MulFix( metrics->horiAdvance,
                                            font_matrix.xx );
          metrics->vertAdvance = FT_MulFix( metrics->vertAdvance,
                                            font_matrix.yy );
        }

        if ( font_offset.x || font_offset.y )
        {
          FT_Outline_Translate( &glyph->root.outline,
                                font_offset.x,
                                font_offset.y );

          metrics->horiAdvance += font_offset.x;
          metrics->vertAdvance += font_offset.y;
        }

        if ( ( load_flags & FT_LOAD_NO_SCALE ) == 0 || force_scaling )
        {
          /* scale the outline and the metrics */
          FT_Int       n;
          FT_Outline*  cur     = &glyph->root.outline;
          FT_Vector*   vec     = cur->points;
          FT_Fixed     x_scale = glyph->x_scale;
          FT_Fixed     y_scale = glyph->y_scale;


          /* First of all, scale the points */
          if ( !hinting || !decoder.builder.hints_funcs )
            for ( n = cur->n_points; n > 0; n--, vec++ )
            {
              vec->x = FT_MulFix( vec->x, x_scale );
              vec->y = FT_MulFix( vec->y, y_scale );
            }

          /* Then scale the metrics */
          metrics->horiAdvance = FT_MulFix( metrics->horiAdvance, x_scale );
          metrics->vertAdvance = FT_MulFix( metrics->vertAdvance, y_scale );
        }

        /* compute the other metrics */
        FT_Outline_Get_CBox( &glyph->root.outline, &cbox );

        metrics->width  = cbox.xMax - cbox.xMin;
        metrics->height = cbox.yMax - cbox.yMin;

        metrics->horiBearingX = cbox.xMin;
        metrics->horiBearingY = cbox.yMax;

        if ( has_vertical_info )
          metrics->vertBearingX = metrics->horiBearingX -
                                    metrics->horiAdvance / 2;
        else
        {
          if ( load_flags & FT_LOAD_VERTICAL_LAYOUT )
            ft_synthesize_vertical_metrics( metrics,
                                            metrics->vertAdvance );
        }
      }
    }

    return error;
  }


/* END */

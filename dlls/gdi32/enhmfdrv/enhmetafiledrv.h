/*
 * Enhanced MetaFile driver definitions
 *
 * Copyright 1999 Huw D M Davies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_ENHMETAFILEDRV_H
#define __WINE_ENHMETAFILEDRV_H

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "ntgdi_private.h"
#include "gdi_private.h"

/* Enhanced Metafile driver physical DC */

typedef struct
{
    struct gdi_physdev dev;
    ENHMETAHEADER  *emh;           /* Pointer to enhanced metafile header */
    DC_ATTR   *dc_attr;
    UINT       handles_size, cur_handles;
    HGDIOBJ   *handles;
    HANDLE     hFile;              /* Handle for disk based MetaFile */
    HBRUSH     dc_brush;
    HPEN       dc_pen;
    BOOL       path;
    INT        dev_caps[COLORMGMTCAPS + 1];
} EMFDRV_PDEVICE;

static inline EMFDRV_PDEVICE *get_emf_physdev( PHYSDEV dev )
{
    return CONTAINING_RECORD( dev, EMFDRV_PDEVICE, dev );
}

extern BOOL EMFDRV_WriteRecord( PHYSDEV dev, EMR *emr ) DECLSPEC_HIDDEN;
extern void EMFDRV_UpdateBBox( PHYSDEV dev, RECTL *rect ) DECLSPEC_HIDDEN;
extern DWORD EMFDRV_CreateBrushIndirect( PHYSDEV dev, HBRUSH hBrush ) DECLSPEC_HIDDEN;

#define HANDLE_LIST_INC 20


#endif  /* __WINE_METAFILEDRV_H */

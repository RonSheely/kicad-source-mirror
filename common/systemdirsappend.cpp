/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2014 CERN
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
 * @author Maciej Suminski <maciej.suminski@cern.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <wx/stdpaths.h>

#include <systemdirsappend.h>
#include <common.h>
#include <kiplatform/environment.h>
#include <search_stack.h>
#include <pgm_base.h>
#include <config.h>     // to define DEFAULT_INSTALL_PATH
#include <paths.h>

// put your best guesses in here, send the computer on a wild goose chase, its
// got nothing else to do.

void SystemDirsAppend( SEARCH_STACK* aSearchStack )
{
    // No clearing is done here, the most general approach is NOT to assume that
    // our appends will be the only thing in the stack.  This function has no
    // knowledge of caller's intentions.

    // wxPathList::AddEnvList() is broken, use SEARCH_STACK::AddPaths().
    // SEARCH_STACK::AddPaths() will verify readability and existence of
    // each directory before adding.
    SEARCH_STACK maybe;

    // User environment variable path is the first search path.  Chances are
    // if the user is savvy enough to set an environment variable they know
    // what they are doing.  It should take precedence over anything else.
    // Otherwise don't set it.
    maybe.AddPaths( wxGetenv( wxT( "KICAD" ) ) );

#ifdef __WXMAC__
    // Add the directory for the user-dependent, program specific data files.
    maybe.AddPaths( PATHS::GetOSXKicadUserDataDir() );

    // Global machine specific application data
    maybe.AddPaths( PATHS::GetOSXKicadMachineDataDir() );

    // Global application specific data files inside bundle
    maybe.AddPaths( PATHS::GetOSXKicadDataDir() );
#else
    // This is from CMAKE_INSTALL_PREFIX.
    // Useful when KiCad is installed by `make install`.
    // Use as second ranked place.
    maybe.AddPaths( wxT( DEFAULT_INSTALL_PATH ) );

#ifdef __WXGTK__
    // On Linux, the stock EDA library data install path can be redefined via
    // KICAD_LIBRARY_DATA, otherwise KICAD_DATA will be used.
    // Useful when multiple versions of KiCad are installed in parallel.
    maybe.AddPaths( PATHS::GetStockEDALibraryPath() );
#endif

    // Add the directory for the user-dependent, program specific data files.
    // According to wxWidgets documentation:
    // Unix: ~/.appname
    // Windows: C:\Documents and Settings\username\Application Data\appname
    maybe.AddPaths( KIPLATFORM::ENV::GetDocumentsPath() );

    {
        // Should be full path to this program executable.
        wxString   bin_dir = Pgm().GetExecutablePath();

#if defined(_WIN32)
        // bin_dir uses unix path separator.  So to parse with wxFileName
        // use windows separator, especially important for server inclusion:
        // like: \\myserver\local_path .
        bin_dir.Replace( wxFileName::GetPathSeparator( wxPATH_UNIX ),
                         wxFileName::GetPathSeparator( wxPATH_WIN ) );
#endif

        wxFileName bin_fn( bin_dir, wxEmptyString );

        // Dir of the global (not user-specific), application specific, data files.
        // From wx docs:
        // Unix: prefix/share/appname
        // Windows: the directory where the executable file is located
        // Mac: appname.app/Contents/SharedSupport bundle subdirectory
        wxString data_dir = wxStandardPaths::Get().GetDataDir();

        if( bin_fn.GetPath() != data_dir )
        {
            // add data_dir if it is different from the bin_dir
            maybe.AddPaths( data_dir );
        }

        // Up one level relative to binary path with "share" appended below.
        bin_fn.RemoveLastDir();
        maybe.AddPaths( bin_fn.GetPath() );
    }

    /* The normal OS program file install paths allow for a binary to be
     * installed in a different path from the library files.  This is
     * useful for development purposes so the library and documentation
     * files do not need to be installed separately.  If someone can
     * figure out a way to implement this without #ifdef, please do.
     */
#if defined(_WIN32)
    maybe.AddPaths( wxGetenv( wxT( "PROGRAMFILES" ) ) );
#else
    maybe.AddPaths( wxGetenv( wxT( "PATH" ) ) );
#endif
#endif

#if defined(DEBUG) && 0
    maybe.Show( "maybe wish list" );
#endif

    // Append 1) kicad, 2) kicad/share, 3) share, and 4) share/kicad to each
    // possible base path in 'maybe'. Since SEARCH_STACK::AddPaths() will verify
    // readability and existence of each directory, not all of these will be
    // actually appended.
    for( unsigned i = 0; i < maybe.GetCount();  ++i )
    {
        wxFileName fn( maybe[i], wxEmptyString );

#ifndef __WXMAC__
        if( fn.GetPath().AfterLast( fn.GetPathSeparator() ) == wxT( "bin" ) )
        {
            fn.RemoveLastDir();

            if( !fn.GetDirCount() )
                continue;               // at least on linux
        }
#endif

        aSearchStack->AddPaths( fn.GetPath() );

#ifndef __WXMAC__
        fn.AppendDir( wxT( "kicad" ) );
        aSearchStack->AddPaths( fn.GetPath() );     // add maybe[i]/kicad

        fn.AppendDir( wxT( "share" ) );
        aSearchStack->AddPaths( fn.GetPath() );     // add maybe[i]/kicad/share

        fn.RemoveLastDir();                         // ../  clear share
        fn.RemoveLastDir();                         // ../  clear kicad

        fn.AppendDir( wxT( "share" ) );
        aSearchStack->AddPaths( fn.GetPath() );     // add maybe[i]/share

        fn.AppendDir( wxT( "kicad" ) );
        aSearchStack->AddPaths( fn.GetPath() );     // add maybe[i]/share/kicad
#endif
    }

#if defined(DEBUG) && 0
    // final results:
    aSearchStack->Show( __func__ );
#endif
}


void GlobalPathsAppend( SEARCH_STACK* aDst, KIWAY::FACE_T aId )
{
    SEARCH_STACK bases;

    SystemDirsAppend( &bases );
    aDst->Clear();

    for( unsigned i = 0; i < bases.GetCount(); ++i )
    {
        wxFileName fn( bases[i], wxEmptyString );

        // Add schematic library file path to search path list.
        // we must add <kicad path>/library and <kicad path>/library/doc
        if( aId == KIWAY::FACE_SCH )
        {
            // Add schematic doc file path (library/doc) to search path list.

            fn.AppendDir( wxT( "library" ) );
            aDst->AddPaths( fn.GetPath() );

            fn.AppendDir( wxT( "doc" ) );
            aDst->AddPaths( fn.GetPath() );

            fn.RemoveLastDir();
            fn.RemoveLastDir(); // "../../"  up twice, removing library/doc/

            fn.AppendDir( wxT( "symbols" ) );
            aDst->AddPaths( fn.GetPath() );

            fn.AppendDir( wxT( "doc" ) );
            aDst->AddPaths( fn.GetPath() );

            fn.RemoveLastDir();
            fn.RemoveLastDir(); // "../../"  up twice, removing symbols/doc/
        }

        // Add PCB library file path to search path list.
        if( aId == KIWAY::FACE_PCB || aId == KIWAY::FACE_CVPCB )
        {
            fn.AppendDir( wxT( "modules" ) );
            aDst->AddPaths( fn.GetPath() );
            fn.RemoveLastDir();

            fn.AppendDir( wxT( "footprints" ) );
            aDst->AddPaths( fn.GetPath() );
            fn.RemoveLastDir();

            // Add 3D module library file path to search path list.
            fn.AppendDir( wxT( "3dmodels" ) );
            aDst->AddPaths( fn.GetPath() );
            fn.RemoveLastDir();
        }

        // Add KiCad template file path to search path list.
        fn.AppendDir( wxT( "template" ) );
        aDst->AddPaths( fn.GetPath() );
    }

#ifndef __WXMAC__
    aDst->AddPaths( wxT( "/usr/local/share" ) );
#endif
}
/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2015 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright (C) 2015 CERN
 * Copyright (C) 2012 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright (C) 2011 Wayne Stambaugh <stambaughw@gmail.com>
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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


#include <common.h>                         // for PAGE_INFO

#include <base_units.h>
#include <board.h>
#include <board_design_settings.h>
#include <component_classes/component_class.h>
#include <netinfo.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_group.h>
#include <pcb_track.h>
#include <zone.h>
#include <string_utils.h>
#include <pcbnew_settings.h>
#include <pcb_edit_frame.h>
#include <netlist_reader/pcb_netlist.h>
#include <connectivity/connectivity_data.h>
#include <reporter.h>

#include "board_netlist_updater.h"


BOARD_NETLIST_UPDATER::BOARD_NETLIST_UPDATER( PCB_EDIT_FRAME* aFrame, BOARD* aBoard ) :
    m_frame( aFrame ),
    m_commit( aFrame ),
    m_board( aBoard )
{
    m_reporter = &NULL_REPORTER::GetInstance();

    m_deleteUnusedFootprints = false;
    m_isDryRun = false;
    m_replaceFootprints = true;
    m_lookupByTimestamp = false;
    m_transferGroups = false;
    m_overrideLocks = false;
    m_updateFields = false;
    m_removeExtraFields = false;

    m_warningCount = 0;
    m_errorCount = 0;
    m_newFootprintsCount = 0;
}


BOARD_NETLIST_UPDATER::~BOARD_NETLIST_UPDATER()
{
}


// These functions allow inspection of pad nets during dry runs by keeping a cache of
// current pad netnames indexed by pad.

void BOARD_NETLIST_UPDATER::cacheNetname( PAD* aPad, const wxString& aNetname )
{
    m_padNets[ aPad ] = aNetname;
}


wxString BOARD_NETLIST_UPDATER::getNetname( PAD* aPad )
{
    if( m_isDryRun && m_padNets.count( aPad ) )
        return m_padNets[ aPad ];
    else
        return aPad->GetNetname();
}


void BOARD_NETLIST_UPDATER::cachePinFunction( PAD* aPad, const wxString& aPinFunction )
{
    m_padPinFunctions[ aPad ] = aPinFunction;
}


wxString BOARD_NETLIST_UPDATER::getPinFunction( PAD* aPad )
{
    if( m_isDryRun && m_padPinFunctions.count( aPad ) )
        return m_padPinFunctions[ aPad ];
    else
        return aPad->GetPinFunction();
}


VECTOR2I BOARD_NETLIST_UPDATER::estimateFootprintInsertionPosition()
{
    VECTOR2I bestPosition;

    if( !m_board->IsEmpty() )
    {
        // Position new components below any existing board features.
        BOX2I bbox = m_board->GetBoardEdgesBoundingBox();

        if( bbox.GetWidth() || bbox.GetHeight() )
        {
            bestPosition.x = bbox.Centre().x;
            bestPosition.y = bbox.GetBottom() + pcbIUScale.mmToIU( 10 );
        }
    }
    else
    {
        // Position new components in the center of the page when the board is empty.
        VECTOR2I pageSize = m_board->GetPageSettings().GetSizeIU( pcbIUScale.IU_PER_MILS );

        bestPosition.x = pageSize.x / 2;
        bestPosition.y = pageSize.y / 2;
    }

    return bestPosition;
}


FOOTPRINT* BOARD_NETLIST_UPDATER::addNewFootprint( COMPONENT* aComponent )
{
    wxString msg;

    if( aComponent->GetFPID().empty() )
    {
        msg.Printf( _( "Cannot add %s (no footprint assigned)." ),
                    aComponent->GetReference() );
        m_reporter->Report( msg, RPT_SEVERITY_ERROR );
        ++m_errorCount;
        return nullptr;
    }

    FOOTPRINT* footprint = m_frame->LoadFootprint( aComponent->GetFPID() );

    if( footprint == nullptr )
    {
        msg.Printf( _( "Cannot add %s (footprint '%s' not found)." ),
                    aComponent->GetReference(),
                    EscapeHTML( aComponent->GetFPID().Format().wx_str() ) );
        m_reporter->Report( msg, RPT_SEVERITY_ERROR );
        ++m_errorCount;
        return nullptr;
    }

    footprint->SetStaticComponentClass(
            m_board->GetComponentClassManager().GetNoneComponentClass() );

    if( m_isDryRun )
    {
        msg.Printf( _( "Add %s (footprint '%s')." ),
                    aComponent->GetReference(),
                    EscapeHTML( aComponent->GetFPID().Format().wx_str() ) );

        delete footprint;
        footprint = nullptr;
    }
    else
    {
        for( PAD* pad : footprint->Pads() )
        {
            // Set the pads ratsnest settings to the global settings
            pad->SetLocalRatsnestVisible( m_frame->GetPcbNewSettings()->m_Display.m_ShowGlobalRatsnest );

            // Pads in the library all have orphaned nets.  Replace with Default.
            pad->SetNetCode( 0 );
        }

        footprint->SetParent( m_board );
        footprint->SetPosition( estimateFootprintInsertionPosition() );

        // This flag is used to prevent connectivity from considering the footprint during its
        // initial build after the footprint is committed, because we're going to immediately start
        // a move operation on the footprint and don't want its pads to drive nets onto vias/tracks
        // it happens to land on at the initial position.
        footprint->SetAttributes( footprint->GetAttributes() | FP_JUST_ADDED );

        m_addedFootprints.push_back( footprint );
        m_commit.Add( footprint );

        msg.Printf( _( "Added %s (footprint '%s')." ),
                    aComponent->GetReference(),
                    EscapeHTML( aComponent->GetFPID().Format().wx_str() ) );
    }

    m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    m_newFootprintsCount++;
    return footprint;
}


void BOARD_NETLIST_UPDATER::updateComponentClass( FOOTPRINT* aFootprint, COMPONENT* aNewComponent )
{
    wxString         curClassName, newClassName;
    COMPONENT_CLASS* newClass = nullptr;

    if( const COMPONENT_CLASS* curClass = aFootprint->GetStaticComponentClass() )
        curClassName = curClass->GetName();

    // Calculate the new component class
    if( m_isDryRun )
    {
        newClassName = COMPONENT_CLASS_MANAGER::GetFullClassNameForConstituents(
                aNewComponent->GetComponentClassNames() );
    }
    else
    {
        newClass = m_board->GetComponentClassManager().GetEffectiveStaticComponentClass(
                aNewComponent->GetComponentClassNames() );
        newClassName = newClass->GetName();
    }

    if( curClassName == newClassName )
        return;

    wxString msg;

    if( m_isDryRun )
    {
        if( curClassName == wxEmptyString && newClassName != wxEmptyString )
        {
            msg.Printf( _( "Change %s component class to '%s'." ),
                        aFootprint->GetReference(),
                        EscapeHTML( newClassName ) );
        }
        else if( curClassName != wxEmptyString && newClassName == wxEmptyString )
        {
            msg.Printf( _( "Remove %s component class (currently '%s')." ),
                        aFootprint->GetReference(),
                        EscapeHTML( curClassName ) );
        }
        else
        {
            msg.Printf( _( "Change %s component class from '%s' to '%s'." ),
                        aFootprint->GetReference(),
                        EscapeHTML( curClassName ),
                        EscapeHTML( newClassName ) );
        }
    }
    else
    {
        wxASSERT_MSG( newClass != nullptr, "Component class should not be nullptr" );

        aFootprint->SetStaticComponentClass( newClass );

        if( curClassName == wxEmptyString && newClassName != wxEmptyString )
        {
            msg.Printf( _( "Changed %s component class to '%s'." ),
                        aFootprint->GetReference(),
                        EscapeHTML( newClassName ) );
        }
        else if( curClassName != wxEmptyString && newClassName == wxEmptyString )
        {
            msg.Printf( _( "Removed %s component class (was '%s')." ),
                        aFootprint->GetReference(),
                        EscapeHTML( curClassName ) );
        }
        else
        {
            msg.Printf( _( "Changed %s component class from '%s' to '%s'." ),
                        aFootprint->GetReference(),
                        EscapeHTML( curClassName ),
                        EscapeHTML( newClassName ) );
        }
    }

    m_reporter->Report( msg, RPT_SEVERITY_ACTION );
}


FOOTPRINT* BOARD_NETLIST_UPDATER::replaceFootprint( NETLIST& aNetlist, FOOTPRINT* aFootprint,
                                                    COMPONENT* aNewComponent )
{
    wxString msg;

    if( aNewComponent->GetFPID().empty() )
    {
        msg.Printf( _( "Cannot update %s (no footprint assigned)." ),
                    aNewComponent->GetReference() );
        m_reporter->Report( msg, RPT_SEVERITY_ERROR );
        ++m_errorCount;
        return nullptr;
    }

    FOOTPRINT* newFootprint = m_frame->LoadFootprint( aNewComponent->GetFPID() );

    if( newFootprint == nullptr )
    {
        msg.Printf( _( "Cannot update %s (footprint '%s' not found)." ),
                    aNewComponent->GetReference(),
                    EscapeHTML( aNewComponent->GetFPID().Format().wx_str() ) );
        m_reporter->Report( msg, RPT_SEVERITY_ERROR );
        ++m_errorCount;
        return nullptr;
    }

    if( m_isDryRun )
    {
        if( aFootprint->IsLocked() && !m_overrideLocks )
        {
            msg.Printf( _( "Cannot change %s footprint from '%s' to '%s' (footprint is locked)."),
                        aFootprint->GetReference(),
                        EscapeHTML( aFootprint->GetFPID().Format().wx_str() ),
                        EscapeHTML( aNewComponent->GetFPID().Format().wx_str() ) );
            m_reporter->Report( msg, RPT_SEVERITY_WARNING );
            ++m_warningCount;
            delete newFootprint;
            return nullptr;
        }
        else
        {
            msg.Printf( _( "Change %s footprint from '%s' to '%s'."),
                        aFootprint->GetReference(),
                        EscapeHTML( aFootprint->GetFPID().Format().wx_str() ),
                        EscapeHTML( aNewComponent->GetFPID().Format().wx_str() ) );
            m_reporter->Report( msg, RPT_SEVERITY_ACTION );
            ++m_newFootprintsCount;
            delete newFootprint;
            return nullptr;
        }
    }
    else
    {
        if( aFootprint->IsLocked() && !m_overrideLocks )
        {
            msg.Printf( _( "Could not change %s footprint from '%s' to '%s' (footprint is locked)."),
                        aFootprint->GetReference(),
                        EscapeHTML( aFootprint->GetFPID().Format().wx_str() ),
                        EscapeHTML( aNewComponent->GetFPID().Format().wx_str() ) );
            m_reporter->Report( msg, RPT_SEVERITY_WARNING );
            ++m_warningCount;
            delete newFootprint;
            return nullptr;
        }
        else
        {
            m_frame->ExchangeFootprint( aFootprint, newFootprint, m_commit );

            msg.Printf( _( "Changed %s footprint from '%s' to '%s'."),
                        aFootprint->GetReference(),
                        EscapeHTML( aFootprint->GetFPID().Format().wx_str() ),
                        EscapeHTML( aNewComponent->GetFPID().Format().wx_str() ) );
            m_reporter->Report( msg, RPT_SEVERITY_ACTION );
            ++m_newFootprintsCount;
            return newFootprint;
        }
    }
}


bool BOARD_NETLIST_UPDATER::updateFootprintParameters( FOOTPRINT* aPcbFootprint,
                                                       COMPONENT* aNetlistComponent )
{
    wxString msg;

    // Create a copy only if the footprint has not been added during this update
    FOOTPRINT* copy = nullptr;

    if( !m_commit.GetStatus( aPcbFootprint ) )
    {
        copy = static_cast<FOOTPRINT*>( aPcbFootprint->Clone() );
        copy->SetParentGroup( nullptr );
    }

    bool       changed = false;

    // Test for reference designator field change.
    if( aPcbFootprint->GetReference() != aNetlistComponent->GetReference() )
    {
        if( m_isDryRun )
        {
            msg.Printf( _( "Change %s reference designator to %s." ),
                        aPcbFootprint->GetReference(),
                        aNetlistComponent->GetReference() );
        }
        else
        {
            msg.Printf( _( "Changed %s reference designator to %s." ),
                        aPcbFootprint->GetReference(),
                        aNetlistComponent->GetReference() );

            changed = true;
            aPcbFootprint->SetReference( aNetlistComponent->GetReference() );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    // Test for value field change.
    if( aPcbFootprint->GetValue() != aNetlistComponent->GetValue() )
    {
        if( m_isDryRun )
        {
            msg.Printf( _( "Change %s value from %s to %s." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( aPcbFootprint->GetValue() ),
                        EscapeHTML( aNetlistComponent->GetValue() ) );
        }
        else
        {
            msg.Printf( _( "Changed %s value from %s to %s." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( aPcbFootprint->GetValue() ),
                        EscapeHTML( aNetlistComponent->GetValue() ) );

            changed = true;
            aPcbFootprint->SetValue( aNetlistComponent->GetValue() );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    // Test for time stamp change.
    KIID_PATH new_path = aNetlistComponent->GetPath();

    if( !aNetlistComponent->GetKIIDs().empty() )
        new_path.push_back( aNetlistComponent->GetKIIDs().front() );

    if( aPcbFootprint->GetPath() != new_path )
    {
        if( m_isDryRun )
        {
            msg.Printf( _( "Update %s symbol association from %s to %s." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( aPcbFootprint->GetPath().AsString() ),
                        EscapeHTML( new_path.AsString() ) );
        }
        else
        {
            msg.Printf( _( "Updated %s symbol association from %s to %s." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( aPcbFootprint->GetPath().AsString() ),
                        EscapeHTML( new_path.AsString() ) );

            changed = true;
            aPcbFootprint->SetPath( new_path );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    nlohmann::ordered_map<wxString, wxString> fpFieldsAsMap;

    for( PCB_FIELD* field : aPcbFootprint->GetFields() )
    {
        // These fields are individually checked above
        if( field->IsReference() || field->IsValue() || field->IsComponentClass() )
        {
            continue;
        }

        fpFieldsAsMap[field->GetName()] = field->GetText();
    }

    // Remove the ref/value/footprint fields that are individually handled
    nlohmann::ordered_map<wxString, wxString> compFields = aNetlistComponent->GetFields();
    compFields.erase( GetCanonicalFieldName( FIELD_T::REFERENCE ) );
    compFields.erase( GetCanonicalFieldName( FIELD_T::VALUE ) );
    compFields.erase( GetCanonicalFieldName( FIELD_T::FOOTPRINT ) );

    // Remove any component class fields - these are not editable in the pcb editor
    compFields.erase( wxT( "Component Class" ) );

    // Fields are stored as an ordered map, but we don't (yet) support reordering
    // the footprint fields to match the symbol, so we manually check the fields
    // in the order they are stored in the symbol.
    bool same = true;

    for( const auto& [name, value] : compFields )
    {
        if( fpFieldsAsMap.count( name ) == 0 || fpFieldsAsMap[name] != value )
        {
            same = false;
            break;
        }
    }

    for( const auto& [name, value] : fpFieldsAsMap )
    {
        if( compFields.count( name ) == 0 )
        {
            same = false;
            break;
        }
    }

    if( !same )
    {
        if( m_isDryRun )
        {
            if( m_updateFields )
            {
                msg.Printf( _( "Update %s fields." ), aPcbFootprint->GetReference() );
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );
            }

            // Remove fields that aren't present in the symbol
            for( PCB_FIELD* field : aPcbFootprint->GetFields() )
            {
                if( field->IsMandatory() )
                    continue;

                if( compFields.count( field->GetName() ) == 0 )
                {
                    if( m_removeExtraFields )
                    {
                        msg.Printf( _( "Remove %s footprint fields not in symbol." ),
                                    aPcbFootprint->GetReference() );
                        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
                    }

                    break;
                }
            }
        }
        else
        {
            if( m_updateFields )
            {
                msg.Printf( _( "Updated %s fields." ), aPcbFootprint->GetReference() );
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );

                changed = true;

                // Add or change field value
                for( auto& [name, value] : compFields )
                {
                    if( aPcbFootprint->HasField( name ) )
                    {
                        aPcbFootprint->GetField( name )->SetText( value );
                    }
                    else
                    {
                        PCB_FIELD* newField = new PCB_FIELD( aPcbFootprint, FIELD_T::USER );
                        aPcbFootprint->Add( newField );

                        newField->SetName( name );
                        newField->SetText( value );
                        newField->SetVisible( false );
                        newField->SetLayer( aPcbFootprint->GetLayer() == F_Cu ? F_Fab : B_Fab );

                        // Give the relative position (0,0) in footprint
                        newField->SetPosition( aPcbFootprint->GetPosition() );
                        // Give the footprint orientation
                        newField->Rotate( aPcbFootprint->GetPosition(), aPcbFootprint->GetOrientation() );

                        if( m_frame )
                            newField->StyleFromSettings( m_frame->GetDesignSettings() );
                    }
                }
            }

            if( m_removeExtraFields )
            {
                bool warned = false;

                std::vector<PCB_FIELD*> fieldList;
                aPcbFootprint->GetFields( fieldList, false );

                for( PCB_FIELD* field : fieldList )
                {
                    if( field->IsMandatory() )
                        continue;

                    if( compFields.count( field->GetName() ) == 0 )
                    {
                        if( !warned )
                        {
                            warned = true;
                            msg.Printf( _( "Removed %s footprint fields not in symbol." ),
                                        aPcbFootprint->GetReference() );
                            m_reporter->Report( msg, RPT_SEVERITY_ACTION );
                        }

                        aPcbFootprint->Remove( field );

                        if( m_frame )
                            m_frame->GetCanvas()->GetView()->Remove( field );

                        delete field;
                    }
                }
            }
        }
    }

    wxString sheetname;
    wxString sheetfile;
    wxString fpFilters;

    wxString humanSheetPath = aNetlistComponent->GetHumanReadablePath();

    if( !humanSheetPath.empty() )
        sheetname = humanSheetPath;
    else if( aNetlistComponent->GetProperties().count( wxT( "Sheetname" ) ) > 0 )
        sheetname = aNetlistComponent->GetProperties().at( wxT( "Sheetname" ) );

    if( aNetlistComponent->GetProperties().count( wxT( "Sheetfile" ) ) > 0 )
        sheetfile = aNetlistComponent->GetProperties().at( wxT( "Sheetfile" ) );

    if( aNetlistComponent->GetProperties().count( wxT( "ki_fp_filters" ) ) > 0 )
        fpFilters = aNetlistComponent->GetProperties().at( wxT( "ki_fp_filters" ) );

    if( sheetname != aPcbFootprint->GetSheetname() )
    {
        if( m_isDryRun )
        {
            msg.Printf( _( "Update %s sheetname to '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( sheetname ) );
        }
        else
        {
            aPcbFootprint->SetSheetname( sheetname );
            msg.Printf( _( "Updated %s sheetname to '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( sheetname ) );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    if( sheetfile != aPcbFootprint->GetSheetfile() )
    {
        if( m_isDryRun )
        {
            msg.Printf( _( "Update %s sheetfile to '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( sheetfile ) );
        }
        else
        {
            aPcbFootprint->SetSheetfile( sheetfile );
            msg.Printf( _( "Updated %s sheetfile to '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( sheetfile ) );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    if( fpFilters != aPcbFootprint->GetFilters() )
    {
        if( m_isDryRun )
        {
            msg.Printf( _( "Update %s footprint filters to '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( fpFilters ) );
        }
        else
        {
            aPcbFootprint->SetFilters( fpFilters );
            msg.Printf( _( "Updated %s footprint filters to '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( fpFilters ) );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    if( ( aNetlistComponent->GetProperties().count( wxT( "exclude_from_bom" ) ) > 0 )
            != ( ( aPcbFootprint->GetAttributes() & FP_EXCLUDE_FROM_BOM ) > 0 ) )
    {
        if( m_isDryRun )
        {
            if( aNetlistComponent->GetProperties().count( wxT( "exclude_from_bom" ) ) )
            {
                msg.Printf( _( "Add %s 'exclude from BOM' fabrication attribute." ),
                            aPcbFootprint->GetReference() );
            }
            else
            {
                msg.Printf( _( "Remove %s 'exclude from BOM' fabrication attribute." ),
                            aPcbFootprint->GetReference() );
            }
        }
        else
        {
            int attributes = aPcbFootprint->GetAttributes();

            if( aNetlistComponent->GetProperties().count( wxT( "exclude_from_bom" ) ) )
            {
                attributes |= FP_EXCLUDE_FROM_BOM;
                msg.Printf( _( "Added %s 'exclude from BOM' fabrication attribute." ),
                            aPcbFootprint->GetReference() );
            }
            else
            {
                attributes &= ~FP_EXCLUDE_FROM_BOM;
                msg.Printf( _( "Removed %s 'exclude from BOM' fabrication attribute." ),
                            aPcbFootprint->GetReference() );
            }

            changed = true;
            aPcbFootprint->SetAttributes( attributes );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    if( ( aNetlistComponent->GetProperties().count( wxT( "dnp" ) ) > 0 )
            != ( ( aPcbFootprint->GetAttributes() & FP_DNP ) > 0 ) )
    {
        if( m_isDryRun )
        {
            if( aNetlistComponent->GetProperties().count( wxT( "dnp" ) ) )
            {
                msg.Printf( _( "Add %s 'Do not place' fabrication attribute." ),
                            aPcbFootprint->GetReference() );
            }
            else
            {
                msg.Printf( _( "Remove %s 'Do not place' fabrication attribute." ),
                            aPcbFootprint->GetReference() );
            }
        }
        else
        {
            int attributes = aPcbFootprint->GetAttributes();

            if( aNetlistComponent->GetProperties().count( wxT( "dnp" ) ) )
            {
                attributes |= FP_DNP;
                msg.Printf( _( "Added %s 'Do not place' fabrication attribute." ),
                            aPcbFootprint->GetReference() );
            }
            else
            {
                attributes &= ~FP_DNP;
                msg.Printf( _( "Removed %s 'Do not place' fabrication attribute." ),
                            aPcbFootprint->GetReference() );
            }

            changed = true;
            aPcbFootprint->SetAttributes( attributes );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    if( aNetlistComponent->GetDuplicatePadNumbersAreJumpers()
        != aPcbFootprint->GetDuplicatePadNumbersAreJumpers() )
    {
        bool value = aNetlistComponent->GetDuplicatePadNumbersAreJumpers();

        if( !m_isDryRun )
        {
            changed = true;
            aPcbFootprint->SetDuplicatePadNumbersAreJumpers( value );

            if( value )
            {
                msg.Printf( _( "Added %s 'duplicate pad numbers are jumpers' attribute." ),
                                aPcbFootprint->GetReference() );
            }
            else
            {
                msg.Printf( _( "Removed %s 'duplicate pad numbers are jumpers' attribute." ),
                                aPcbFootprint->GetReference() );
            }
        }
        else
        {
            if( value )
            {
                msg.Printf( _( "Add %s 'duplicate pad numbers are jumpers' attribute." ),
                                aPcbFootprint->GetReference() );
            }
            else
            {
                msg.Printf( _( "Remove %s 'duplicate pad numbers are jumpers' attribute." ),
                                aPcbFootprint->GetReference() );
            }
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    if( aNetlistComponent->JumperPadGroups() != aPcbFootprint->JumperPadGroups() )
    {
        if( !m_isDryRun )
        {
            changed = true;
            aPcbFootprint->JumperPadGroups() = aNetlistComponent->JumperPadGroups();
            msg.Printf( _( "Updated %s jumper pad groups" ), aPcbFootprint->GetReference() );
        }
        else
        {
            msg.Printf( _( "Update %s jumper pad groups" ), aPcbFootprint->GetReference() );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    if( changed && copy )
        m_commit.Modified( aPcbFootprint, copy );
    else
        delete copy;

    return true;
}


bool BOARD_NETLIST_UPDATER::updateFootprintGroup( FOOTPRINT* aPcbFootprint,
                                                  COMPONENT* aNetlistComponent )
{
    if( !m_transferGroups )
        return false;

    wxString msg;

    // Create a copy only if the footprint has not been added during this update
    FOOTPRINT* copy = nullptr;

    if( !m_commit.GetStatus( aPcbFootprint ) )
    {
        copy = static_cast<FOOTPRINT*>( aPcbFootprint->Clone() );
        copy->SetParentGroup( nullptr );
    }

    bool changed = false;

    // These hold the info for group and group KIID coming from the netlist
    // newGroup may point to an existing group on the board if we find an
    // incoming group UUID that matches an existing group
    PCB_GROUP* newGroup = nullptr;
    KIID       newGroupKIID = aNetlistComponent->GetGroup() ? aNetlistComponent->GetGroup()->uuid : 0;

    PCB_GROUP* existingGroup = static_cast<PCB_GROUP*>( aPcbFootprint->GetParentGroup() );
    KIID       existingGroupKIID = existingGroup ? existingGroup->m_Uuid : 0;

    // Find existing group based on matching UUIDs
    auto it = std::find_if( m_board->Groups().begin(), m_board->Groups().end(),
                [&](PCB_GROUP* group) {
                    return group->m_Uuid == newGroupKIID;
                });

    // If we find a group with the same UUID, use it
    if( it != m_board->Groups().end() )
        newGroup = *it;

    // No changes, nothing to do
    if( newGroupKIID == existingGroupKIID )
        return changed;

    // Remove from existing group
    if( existingGroupKIID != 0 )
    {
        if( m_isDryRun )
        {
            msg.Printf( _( "Remove %s from group '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( existingGroup->GetName() ) );
        }
        else
        {
            msg.Printf( _( "Removed %s from group '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( existingGroup->GetName() ) );

            changed = true;
            m_commit.Modify( existingGroup, nullptr, RECURSE_MODE::NO_RECURSE );
            existingGroup->RemoveItem( aPcbFootprint );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    // Add to new group
    if( newGroupKIID != 0 )
    {
        if( m_isDryRun )
        {
            msg.Printf( _( "Add %s to group '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( aNetlistComponent->GetGroup()->name ) );
        }
        else
        {
            msg.Printf( _( "Added %s group '%s'." ),
                        aPcbFootprint->GetReference(),
                        EscapeHTML( aNetlistComponent->GetGroup()->name ) );

            changed = true;

            if( newGroup == nullptr )
            {
                newGroup = new PCB_GROUP( m_board );
                const_cast<KIID&>( newGroup->m_Uuid ) = newGroupKIID;
                newGroup->SetName( aNetlistComponent->GetGroup()->name );

                // Add the group to the board manually so we can find it by checking
                // board groups for later footprints that are checking for existing groups
                m_board->Add( newGroup );
                m_commit.Added( newGroup );
            }
            else
            {
                m_commit.Modify( newGroup->AsEdaItem(), nullptr, RECURSE_MODE::NO_RECURSE );
            }

            newGroup->AddItem( aPcbFootprint );
        }

        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
    }

    if( changed && copy )
        m_commit.Modified( aPcbFootprint, copy );
    else if( copy )
        delete copy;

    return changed;
}


bool BOARD_NETLIST_UPDATER::updateComponentPadConnections( FOOTPRINT* aFootprint,
                                                           COMPONENT* aNewComponent )
{
    wxString msg;

    // Create a copy only if the footprint has not been added during this update
    FOOTPRINT* copy = nullptr;

    if( !m_isDryRun && !m_commit.GetStatus( aFootprint ) )
    {
        copy = static_cast<FOOTPRINT*>( aFootprint->Clone() );
        copy->SetParentGroup( nullptr );
    }

    bool changed = false;

    // At this point, the component footprint is updated.  Now update the nets.
    std::deque<PAD*>   pads = aFootprint->Pads();
    std::set<wxString> padNetnames;

    std::sort( pads.begin(), pads.end(),
               []( PAD* a, PAD* b )
               {
                   return a->m_Uuid < b->m_Uuid;
               } );

    for( PAD* pad : pads )
    {
        const COMPONENT_NET& net = aNewComponent->GetNet( pad->GetNumber() );

        wxString pinFunction;
        wxString pinType;

        if( net.IsValid() )     // i.e. the pad has a name
        {
            pinFunction = net.GetPinFunction();
            pinType = net.GetPinType();
        }

        if( !m_isDryRun )
        {
            if( pad->GetPinFunction() != pinFunction )
            {
                changed = true;
                pad->SetPinFunction( pinFunction );
            }

            if( pad->GetPinType() != pinType )
            {
                changed = true;
                pad->SetPinType( pinType );
            }
        }
        else
        {
            cachePinFunction( pad, pinFunction );
        }

        // Test if new footprint pad has no net (pads not on copper layers have no net).
        if( !net.IsValid() || !pad->IsOnCopperLayer() )
        {
            if( !pad->GetNetname().IsEmpty() )
            {
                if( m_isDryRun )
                {
                    msg.Printf( _( "Disconnect %s pin %s." ),
                                aFootprint->GetReference(),
                                EscapeHTML( pad->GetNumber() ) );
                }
                else
                {
                    msg.Printf( _( "Disconnected %s pin %s." ),
                                aFootprint->GetReference(),
                                EscapeHTML( pad->GetNumber() ) );
                }

                m_reporter->Report( msg, RPT_SEVERITY_ACTION );
            }
            else if( pad->IsOnCopperLayer() && !pad->GetNumber().IsEmpty() )
            {
                // pad is connectable but has no net found in netlist
                msg.Printf( _( "No net found for component %s pad %s (no pin %s in symbol)." ),
                            aFootprint->GetReference(),
                            EscapeHTML( pad->GetNumber() ),
                            EscapeHTML( pad->GetNumber() ) );
                m_reporter->Report( msg, RPT_SEVERITY_WARNING);
                ++m_warningCount;
            }

            if( !m_isDryRun )
            {
                changed = true;
                pad->SetNetCode( NETINFO_LIST::UNCONNECTED );

                // If the pad has no net from netlist (i.e. not in netlist
                // it cannot have a pin function
                if( pad->GetNetname().IsEmpty() )
                    pad->SetPinFunction( wxEmptyString );

            }
            else
            {
                cacheNetname( pad, wxEmptyString );
            }
        }
        else                                 // New footprint pad has a net.
        {
            wxString netName = net.GetNetName();

            if( pad->IsNoConnectPad() )
            {
                netName = wxString::Format( wxS( "%s" ),
                                            EscapeHTML( net.GetNetName() ) );

                for( int jj = 1; !padNetnames.insert( netName ).second; jj++ )
                {
                    netName = wxString::Format( wxS( "%s_%d" ),
                                                EscapeHTML( net.GetNetName() ), jj );
                }
            }

            NETINFO_ITEM* netinfo = m_board->FindNet( netName );

            if( netinfo && !m_isDryRun )
                netinfo->SetIsCurrent( true );

            if( pad->GetNetname() != netName )
            {

                if( netinfo == nullptr )
                {
                    // It might be a new net that has not been added to the board yet
                    if( m_addedNets.count( netName ) )
                        netinfo = m_addedNets[ netName ];
                }

                if( netinfo == nullptr )
                {
                    netinfo = new NETINFO_ITEM( m_board, netName );

                    // It is a new net, we have to add it
                    if( !m_isDryRun )
                    {
                        changed = true;
                        m_commit.Add( netinfo );
                    }

                    m_addedNets[netName] = netinfo;
                    msg.Printf( _( "Add net %s." ),
                                EscapeHTML( UnescapeString( netName ) ) );
                    m_reporter->Report( msg, RPT_SEVERITY_ACTION );
                }

                if( !pad->GetNetname().IsEmpty() )
                {
                    m_oldToNewNets[ pad->GetNetname() ] = netName;

                    if( m_isDryRun )
                    {
                        msg.Printf( _( "Reconnect %s pin %s from %s to %s."),
                                    aFootprint->GetReference(),
                                    EscapeHTML( pad->GetNumber() ),
                                    EscapeHTML( UnescapeString( pad->GetNetname() ) ),
                                    EscapeHTML( UnescapeString( netName ) ) );
                    }
                    else
                    {
                        msg.Printf( _( "Reconnected %s pin %s from %s to %s."),
                                    aFootprint->GetReference(),
                                    EscapeHTML( pad->GetNumber() ),
                                    EscapeHTML( UnescapeString( pad->GetNetname() ) ),
                                    EscapeHTML( UnescapeString( netName ) ) );
                    }
                }
                else
                {
                    if( m_isDryRun )
                    {
                        msg.Printf( _( "Connect %s pin %s to %s."),
                                    aFootprint->GetReference(),
                                    EscapeHTML( pad->GetNumber() ),
                                    EscapeHTML( UnescapeString( netName ) ) );
                    }
                    else
                    {
                        msg.Printf( _( "Connected %s pin %s to %s."),
                                    aFootprint->GetReference(),
                                    EscapeHTML( pad->GetNumber() ),
                                    EscapeHTML( UnescapeString( netName ) ) );
                    }
                }

                m_reporter->Report( msg, RPT_SEVERITY_ACTION );

                if( !m_isDryRun )
                {
                    changed = true;
                    pad->SetNet( netinfo );
                }
                else
                {
                    cacheNetname( pad, netName );
                }
            }
        }
    }

    if( changed && copy )
        m_commit.Modified( aFootprint, copy );
    else if( copy )
        delete copy;

    return true;
}


void BOARD_NETLIST_UPDATER::cacheCopperZoneConnections()
{
    for( ZONE* zone : m_board->Zones() )
    {
        if( !zone->IsOnCopperLayer() || zone->GetIsRuleArea() )
            continue;

        m_zoneConnectionsCache[ zone ] = m_board->GetConnectivity()->GetConnectedPads( zone );
    }
}


bool BOARD_NETLIST_UPDATER::updateCopperZoneNets( NETLIST& aNetlist )
{
    wxString msg;
    std::set<wxString> netlistNetnames;

    for( int ii = 0; ii < (int) aNetlist.GetCount(); ii++ )
    {
        const COMPONENT* component = aNetlist.GetComponent( ii );

        for( unsigned jj = 0; jj < component->GetNetCount(); jj++ )
        {
            const COMPONENT_NET& net = component->GetNet( jj );
            netlistNetnames.insert( net.GetNetName() );
        }
    }

    for( PCB_TRACK* via : m_board->Tracks() )
    {
        if( via->Type() != PCB_VIA_T )
            continue;

        if( netlistNetnames.count( via->GetNetname() ) == 0 )
        {
            wxString updatedNetname = wxEmptyString;

            // Take via name from name change map if it didn't match to a new pad
            // (this is useful for stitching vias that don't connect to tracks)
            if( m_oldToNewNets.count( via->GetNetname() ) )
            {
                updatedNetname = m_oldToNewNets[via->GetNetname()];
            }

            if( !updatedNetname.IsEmpty() )
            {
                if( m_isDryRun )
                {
                    wxString originalNetname = via->GetNetname();

                    msg.Printf( _( "Reconnect via from %s to %s." ),
                                EscapeHTML( UnescapeString( originalNetname ) ),
                                EscapeHTML( UnescapeString( updatedNetname ) ) );

                    m_reporter->Report( msg, RPT_SEVERITY_ACTION );
                }
                else
                {
                    NETINFO_ITEM* netinfo = m_board->FindNet( updatedNetname );

                    if( !netinfo )
                        netinfo = m_addedNets[updatedNetname];

                    if( netinfo )
                    {
                        wxString originalNetname = via->GetNetname();

                        m_commit.Modify( via );
                        via->SetNet( netinfo );

                        msg.Printf( _( "Reconnected via from %s to %s." ),
                                    EscapeHTML( UnescapeString( originalNetname ) ),
                                    EscapeHTML( UnescapeString( updatedNetname ) ) );

                        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
                    }
                }
            }
            else
            {
                msg.Printf( _( "Via connected to unknown net (%s)." ),
                            EscapeHTML( UnescapeString( via->GetNetname() ) ) );
                m_reporter->Report( msg, RPT_SEVERITY_WARNING );
                ++m_warningCount;
            }
        }
    }

    // Test copper zones to detect "dead" nets (nets without any pad):
    for( ZONE* zone : m_board->Zones() )
    {
        if( !zone->IsOnCopperLayer() || zone->GetIsRuleArea() )
            continue;

        if( netlistNetnames.count( zone->GetNetname() ) == 0 )
        {
            // Look for a pad in the zone's connected-pad-cache which has been updated to
            // a new net and use that. While this won't always be the right net, the dead
            // net is guaranteed to be wrong.
            wxString updatedNetname = wxEmptyString;

            for( PAD* pad : m_zoneConnectionsCache[ zone ] )
            {
                if( getNetname( pad ) != zone->GetNetname() )
                {
                    updatedNetname = getNetname( pad );
                    break;
                }
            }

            // Take zone name from name change map if it didn't match to a new pad
            // (this is useful for zones on internal layers)
            if( updatedNetname.IsEmpty() && m_oldToNewNets.count( zone->GetNetname() ) )
            {
                updatedNetname = m_oldToNewNets[ zone->GetNetname() ];
            }

            if( !updatedNetname.IsEmpty() )
            {
                if( m_isDryRun )
                {
                    wxString originalNetname = zone->GetNetname();

                    if( !zone->GetZoneName().IsEmpty() )
                    {
                        msg.Printf( _( "Reconnect copper zone '%s' from %s to %s." ),
                                    zone->GetZoneName(),
                                    EscapeHTML( UnescapeString( originalNetname ) ),
                                    EscapeHTML( UnescapeString( updatedNetname ) ) );
                    }
                    else
                    {
                        msg.Printf( _( "Reconnect copper zone from %s to %s." ),
                                    EscapeHTML( UnescapeString( originalNetname ) ),
                                    EscapeHTML( UnescapeString( updatedNetname ) ) );
                    }

                    m_reporter->Report( msg, RPT_SEVERITY_ACTION );
                }
                else
                {
                    NETINFO_ITEM* netinfo = m_board->FindNet( updatedNetname );

                    if( !netinfo )
                        netinfo = m_addedNets[ updatedNetname ];

                    if( netinfo )
                    {
                        wxString originalNetname = zone->GetNetname();

                        m_commit.Modify( zone );
                        zone->SetNet( netinfo );

                        if( !zone->GetZoneName().IsEmpty() )
                        {
                            msg.Printf( _( "Reconnected copper zone '%s' from %s to %s." ),
                                        EscapeHTML( zone->GetZoneName() ),
                                        EscapeHTML( UnescapeString( originalNetname ) ),
                                        EscapeHTML( UnescapeString( updatedNetname ) ) );
                        }
                        else
                        {
                            msg.Printf( _( "Reconnected copper zone from %s to %s." ),
                                        EscapeHTML( UnescapeString( originalNetname ) ),
                                        EscapeHTML( UnescapeString( updatedNetname ) ) );
                        }

                        m_reporter->Report( msg, RPT_SEVERITY_ACTION );
                    }
                }
            }
            else
            {
                if( !zone->GetZoneName().IsEmpty() )
                {
                    msg.Printf( _( "Copper zone '%s' has no pads connected." ),
                                EscapeHTML( zone->GetZoneName() ) );
                }
                else
                {
                    PCB_LAYER_ID layer = zone->GetLayer();
                    VECTOR2I     pt = zone->GetPosition();

                    if( m_frame && m_frame->GetPcbNewSettings() )
                    {
                        if( m_frame->GetPcbNewSettings()->m_Display.m_DisplayInvertXAxis )
                            pt.x *= -1;

                        if( m_frame->GetPcbNewSettings()->m_Display.m_DisplayInvertYAxis )
                            pt.y *= -1;
                    }

                    msg.Printf( _( "Copper zone on layer %s at (%s, %s) has no pads connected." ),
                                EscapeHTML( m_board->GetLayerName( layer ) ),
                                m_frame ? m_frame->MessageTextFromValue( pt.x )
                                        : EDA_UNIT_UTILS::UI::MessageTextFromValue( pcbIUScale, EDA_UNITS::MM, pt.x ),
                                m_frame ? m_frame->MessageTextFromValue( pt.y )
                                        : EDA_UNIT_UTILS::UI::MessageTextFromValue( pcbIUScale, EDA_UNITS::MM, pt.y ) );
                }

                m_reporter->Report( msg, RPT_SEVERITY_WARNING );
                ++m_warningCount;
            }
        }
    }

    return true;
}


bool BOARD_NETLIST_UPDATER::updateGroups( NETLIST& aNetlist )
{
    if( !m_transferGroups )
        return false;

    for( PCB_GROUP* pcbGroup : m_board->Groups() )
    {
        NETLIST_GROUP* netlistGroup = aNetlist.GetGroupByUuid( pcbGroup->m_Uuid );

        if( netlistGroup == nullptr )
            continue;

        if( netlistGroup->name != pcbGroup->GetName() )
        {
            if( m_isDryRun )
            {
                wxString msg;
                msg.Printf( _( "Change group name to '%s' to '%s'." ), EscapeHTML( pcbGroup->GetName() ),
                            EscapeHTML( netlistGroup->name ) );
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );
            }
            else
            {
                wxString msg;
                msg.Printf( _( "Changed group name to '%s' to '%s'." ), EscapeHTML( pcbGroup->GetName() ),
                            EscapeHTML( netlistGroup->name ) );
                m_commit.Modify( pcbGroup->AsEdaItem(), nullptr, RECURSE_MODE::NO_RECURSE );
                pcbGroup->SetName( netlistGroup->name );
            }
        }

        if( netlistGroup->libId != pcbGroup->GetDesignBlockLibId() )
        {
            if( m_isDryRun )
            {
                wxString msg;
                msg.Printf( _( "Change group library link to '%s'." ),
                            EscapeHTML( netlistGroup->libId.GetUniStringLibId() ) );
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );
            }
            else
            {
                wxString msg;
                msg.Printf( _( "Changed group library link to '%s'." ),
                            EscapeHTML( netlistGroup->libId.GetUniStringLibId() ) );
                m_commit.Modify( pcbGroup->AsEdaItem(), nullptr, RECURSE_MODE::NO_RECURSE );
                pcbGroup->SetDesignBlockLibId( netlistGroup->libId );
            }
        }
    }

    return true;
}


bool BOARD_NETLIST_UPDATER::testConnectivity( NETLIST& aNetlist,
                                              std::map<COMPONENT*, FOOTPRINT*>& aFootprintMap )
{
    // Verify that board contains all pads in netlist: if it doesn't then footprints are
    // wrong or missing.

    wxString msg;
    wxString padNumber;

    for( int i = 0; i < (int) aNetlist.GetCount(); i++ )
    {
        COMPONENT* component = aNetlist.GetComponent( i );
        FOOTPRINT* footprint = aFootprintMap[component];

        if( !footprint )    // It can be missing in partial designs
            continue;

        // Explore all pins/pads in component
        for( unsigned jj = 0; jj < component->GetNetCount(); jj++ )
        {
            padNumber = component->GetNet( jj ).GetPinName();

            if( padNumber.IsEmpty() )
            {
                // bad symbol, report error
                msg.Printf( _( "Symbol %s has pins with no number.  These pins can not be matched "
                               "to pads in %s." ),
                            component->GetReference(),
                            EscapeHTML( footprint->GetFPID().Format().wx_str() ) );
                m_reporter->Report( msg, RPT_SEVERITY_ERROR );
                ++m_errorCount;
            }
            else if( !footprint->FindPadByNumber( padNumber ) )
            {
                // not found: bad footprint, report error
                msg.Printf( _( "%s pad %s not found in %s." ),
                            component->GetReference(),
                            EscapeHTML( padNumber ),
                            EscapeHTML( footprint->GetFPID().Format().wx_str() ) );
                m_reporter->Report( msg, RPT_SEVERITY_ERROR );
                ++m_errorCount;
            }
        }
    }

    return true;
}


bool BOARD_NETLIST_UPDATER::UpdateNetlist( NETLIST& aNetlist )
{
    FOOTPRINT* lastPreexistingFootprint = nullptr;
    COMPONENT* component = nullptr;
    wxString   msg;
    std::unordered_set<wxString> sheetPaths;

    m_errorCount = 0;
    m_warningCount = 0;
    m_newFootprintsCount = 0;

    std::map<COMPONENT*, FOOTPRINT*> footprintMap;

    if( !m_board->Footprints().empty() )
        lastPreexistingFootprint = m_board->Footprints().back();

    cacheCopperZoneConnections();

    // First mark all nets (except <no net>) as stale; we'll update those which are current
    // in the following two loops. Also prepare the component class manager for updates.
    //
    if( !m_isDryRun )
    {
        for( NETINFO_ITEM* net : m_board->GetNetInfo() )
            net->SetIsCurrent( net->GetNetCode() == 0 );

        m_board->GetComponentClassManager().InitNetlistUpdate();
    }

    // Next go through the netlist updating all board footprints which have matching component
    // entries and adding new footprints for those that don't.
    //
    for( unsigned i = 0; i < aNetlist.GetCount(); i++ )
    {
        component = aNetlist.GetComponent( i );

        if( component->GetProperties().count( wxT( "exclude_from_board" ) ) )
            continue;

        msg.Printf( _( "Processing symbol '%s:%s'." ),
                    component->GetReference(),
                    EscapeHTML( component->GetFPID().Format().wx_str() ) );
        m_reporter->Report( msg, RPT_SEVERITY_INFO );

        int matchCount = 0;

        for( FOOTPRINT* footprint : m_board->Footprints() )
        {
            bool match = false;

            if( m_lookupByTimestamp )
            {
                for( const KIID& uuid : component->GetKIIDs() )
                {
                    KIID_PATH base = component->GetPath();
                    base.push_back( uuid );

                    if( footprint->GetPath() == base )
                    {
                        match = true;
                        break;
                    }
                }
            }
            else
            {
                match = footprint->GetReference().CmpNoCase( component->GetReference() ) == 0;
            }

            if( match )
            {
                FOOTPRINT* tmp = footprint;

                if( m_replaceFootprints && component->GetFPID() != footprint->GetFPID() )
                    tmp = replaceFootprint( aNetlist, footprint, component );

                if( !tmp )
                    tmp = footprint;

                if( tmp )
                {
                    footprintMap[ component ] = tmp;

                    updateFootprintParameters( tmp, component );
                    updateFootprintGroup( tmp, component );
                    updateComponentPadConnections( tmp, component );
                    updateComponentClass( tmp, component );

                    sheetPaths.insert( footprint->GetSheetname() );
                }

                matchCount++;
            }

            if( footprint == lastPreexistingFootprint )
            {
                // No sense going through the newly-created footprints: end of loop
                break;
            }
        }

        if( matchCount == 0 )
        {
            FOOTPRINT* footprint = addNewFootprint( component );

            if( footprint )
            {
                footprintMap[ component ] = footprint;

                updateFootprintParameters( footprint, component );
                updateFootprintGroup( footprint, component );
                updateComponentPadConnections( footprint, component );
                updateComponentClass( footprint, component );

                sheetPaths.insert( footprint->GetSheetname() );
            }
        }
        else if( matchCount > 1 )
        {
            msg.Printf( _( "Multiple footprints found for %s." ),
                        component->GetReference() );
            m_reporter->Report( msg, RPT_SEVERITY_ERROR );
            m_errorCount++;
        }
    }

    updateCopperZoneNets( aNetlist );
    updateGroups( aNetlist );

    // Finally go through the board footprints and update all those that *don't* have matching
    // component entries.
    //
    for( FOOTPRINT* footprint : m_board->Footprints() )
    {
        bool matched = false;
        bool doDelete = m_deleteUnusedFootprints;

        if( ( footprint->GetAttributes() & FP_BOARD_ONLY ) > 0 )
            doDelete = false;

        if( m_lookupByTimestamp )
            component = aNetlist.GetComponentByPath( footprint->GetPath() );
        else
            component = aNetlist.GetComponentByReference( footprint->GetReference() );

        if( component && component->GetProperties().count( wxT( "exclude_from_board" ) ) == 0 )
            matched = true;

        if( doDelete && !matched && footprint->IsLocked() && !m_overrideLocks )
        {
            if( m_isDryRun )
            {
                msg.Printf( _( "Cannot remove unused footprint %s (footprint is locked)." ),
                            footprint->GetReference() );
            }
            else
            {
                msg.Printf( _( "Could not remove unused footprint %s (footprint is locked)." ),
                            footprint->GetReference() );
            }

            m_reporter->Report( msg, RPT_SEVERITY_WARNING );
            m_warningCount++;
            doDelete = false;
        }

        if( doDelete && !matched )
        {
            if( m_isDryRun )
            {
                msg.Printf( _( "Remove unused footprint %s." ),
                            footprint->GetReference() );
            }
            else
            {
                m_commit.Remove( footprint );
                msg.Printf( _( "Removed unused footprint %s." ),
                            footprint->GetReference() );
            }

            m_reporter->Report( msg, RPT_SEVERITY_ACTION );
        }
        else if( !m_isDryRun )
        {
            if( !matched )
                footprint->SetPath( KIID_PATH() );

            for( PAD* pad : footprint->Pads() )
            {
                if( pad->GetNet() )
                    pad->GetNet()->SetIsCurrent( true );
            }
        }
    }

    if( !m_isDryRun )
    {
        // Finalise the component class manager
        m_board->GetComponentClassManager().FinishNetlistUpdate();
        m_board->SynchronizeComponentClasses( sheetPaths );

        m_board->BuildConnectivity();
        testConnectivity( aNetlist, footprintMap );

        for( NETINFO_ITEM* net : m_board->GetNetInfo() )
        {
            if( !net->IsCurrent() )
            {
                msg.Printf( _( "Removed unused net %s." ),
                            EscapeHTML( net->GetNetname() ) );
                m_reporter->Report( msg, RPT_SEVERITY_ACTION );
            }
        }

        m_board->RemoveUnusedNets( &m_commit );

        // When new footprints are added, the automatic zone refill is disabled because:
        // * it creates crashes when calculating dynamic ratsnests if auto refill is enabled.
        // (the auto refills rebuild the connectivity with incomplete data)
        // * it is useless because zones will be refilled after placing new footprints
        m_commit.Push( _( "Update Netlist" ), m_newFootprintsCount ? ZONE_FILL_OP  : 0 );

        // Update net, netcode and netclass data after commiting the netlist
        m_board->SynchronizeNetsAndNetClasses( true );
        m_board->GetConnectivity()->RefreshNetcodeMap( m_board );

        // Although m_commit will probably also set this, it's not guaranteed, and we need to make
        // sure any modification to netclasses gets persisted to project settings through a save.
        if( m_frame )
            m_frame->OnModify();
    }

    if( m_isDryRun )
    {
        for( const std::pair<const wxString, NETINFO_ITEM*>& addedNet : m_addedNets )
            delete addedNet.second;

        m_addedNets.clear();
    }

    // Update the ratsnest
    m_reporter->ReportTail( wxT( "" ), RPT_SEVERITY_ACTION );
    m_reporter->ReportTail( wxT( "" ), RPT_SEVERITY_ACTION );

    msg.Printf( _( "Total warnings: %d, errors: %d." ), m_warningCount, m_errorCount );
    m_reporter->ReportTail( msg, RPT_SEVERITY_INFO );

    return true;
}

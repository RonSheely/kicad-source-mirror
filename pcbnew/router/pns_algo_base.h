/*
 * KiRouter - a push-and-(sometimes-)shove PCB router
 *
 * Copyright (C) 2013-2014 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * @author Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __PNS_ALGO_BASE_H
#define __PNS_ALGO_BASE_H

#include <math/box2.h>

#include "pns_routing_settings.h"

namespace PNS {

class ROUTER;
class LOGGER;
class DEBUG_DECORATOR;

/**
 * Base class for all P&S algorithms (shoving, walkaround, line placement, dragging, etc.).
 *
 * Holds a bunch of objects commonly used by all algorithms (P&S settings, parent router
 * instance, logging).
 */
class ALGO_BASE
{
public:
    ALGO_BASE( ROUTER* aRouter ) :
        m_debugDecorator( nullptr ),
        m_router( aRouter ),
        m_logger( nullptr )
    {}

    virtual ~ALGO_BASE() {}

    ///< Return the instance of our router
    ROUTER* Router() const
    {
        return m_router;
    }

    ///< Return current router settings
    ROUTING_SETTINGS& Settings() const;

    ///< Return the logger object, allowing to dump geometry to a file.
    virtual LOGGER* Logger();

    void SetLogger( LOGGER* aLogger )
    {
        m_logger = aLogger;
    }

    /**
     * Assign a debug decorator allowing this algo to draw extra graphics for visual debugging.
     */
    void SetDebugDecorator( DEBUG_DECORATOR* aDecorator )
    {
        m_debugDecorator = aDecorator;
    }

    DEBUG_DECORATOR* Dbg() const
    {
        return m_debugDecorator;
    }

    const BOX2I& VisibleViewArea() const;

protected:
    DEBUG_DECORATOR *m_debugDecorator;
    ROUTER* m_router;
    LOGGER* m_logger;
};

}

#endif

/**
 * NetXMS - open source network management system
 * Copyright (C) 2003-2016 Victor Kirhenshtein
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
package org.netxms.client.constants;

import java.util.HashMap;
import java.util.Map;
import org.netxms.base.Logger;

/**
 * Node type
 */
public enum NodeType
{
   UNKNOWN(0),
   PHYSICAL(1),
   VIRTUAL(2),
   CONTROLLER(3);

   private int value;
   private static Map<Integer, NodeType> lookupTable = new HashMap<Integer, NodeType>();

   static
   {
      for(NodeType element : NodeType.values())
      {
         lookupTable.put(element.value, element);
      }
   }

   /**
    * Internal constructor
    *  
    * @param value integer value
    */
   private NodeType(int value)
   {
      this.value = value;
   }

   /**
    * Get integer value
    * 
    * @return integer value
    */
   public int getValue()
   {
      return value;
   }

   /**
    * Get enum element by integer value
    * 
    * @param value integer value
    * @return enum element corresponding to given integer value or fall-back element for invalid value
    */
   public static NodeType getByValue(int value)
   {
      final NodeType element = lookupTable.get(value);
      if (element == null)
      {
         Logger.warning(NodeType.class.getName(), "Unknown element " + value);
         return UNKNOWN; // fallback
      }
      return element;
   }
}

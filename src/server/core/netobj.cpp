/*
** NetXMS - Network Management System
** Copyright (C) 2003-2016 Victor Kirhenshtein
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** File: netobj.cpp
**
**/

#include "nxcore.h"

/**
 * Class names
 */
static const TCHAR *s_className[]=
   {
      _T("Generic"), _T("Subnet"), _T("Node"), _T("Interface"),
      _T("Network"), _T("Container"), _T("Zone"), _T("ServiceRoot"),
      _T("Template"), _T("TemplateGroup"), _T("TemplateRoot"),
      _T("NetworkService"), _T("VPNConnector"), _T("Condition"),
      _T("Cluster"), _T("PolicyGroup"), _T("PolicyRoot"),
      _T("AgentPolicy"), _T("AgentPolicyConfig"), _T("NetworkMapRoot"),
      _T("NetworkMapGroup"), _T("NetworkMap"), _T("DashboardRoot"),
      _T("Dashboard"), _T("ReportRoot"), _T("ReportGroup"), _T("Report"),
      _T("BusinessServiceRoot"), _T("BusinessService"), _T("NodeLink"),
      _T("ServiceCheck"), _T("MobileDevice"), _T("Rack"), _T("AccessPoint"),
      _T("AgentPolicyLogParser"), _T("Chassis")
   };

/**
 * Default constructor
 */
NetObj::NetObj()
{
   int i;

   m_id = 0;
   m_dwRefCount = 0;
   m_mutexProperties = MutexCreate();
   m_mutexRefCount = MutexCreate();
   m_mutexACL = MutexCreate();
   m_rwlockParentList = RWLockCreate();
   m_rwlockChildList = RWLockCreate();
   m_status = STATUS_UNKNOWN;
   m_name[0] = 0;
   m_comments = NULL;
   m_isModified = false;
   m_isDeleted = false;
   m_isHidden = false;
	m_isSystem = false;
	m_maintenanceMode = false;
	m_maintenanceEventId = 0;
   m_childList = new ObjectArray<NetObj>(0, 16, false);
   m_parentList = new ObjectArray<NetObj>(4, 4, false);
   m_accessList = new AccessList();
   m_inheritAccessRights = true;
	m_dwNumTrustedNodes = 0;
	m_pdwTrustedNodes = NULL;
   m_pollRequestor = NULL;
   m_statusCalcAlg = SA_CALCULATE_DEFAULT;
   m_statusPropAlg = SA_PROPAGATE_DEFAULT;
   m_fixedStatus = STATUS_WARNING;
   m_statusShift = 0;
   m_statusSingleThreshold = 75;
   m_dwTimeStamp = 0;
   for(i = 0; i < 4; i++)
   {
      m_statusTranslation[i] = i + 1;
      m_statusThresholds[i] = 80 - i * 20;
   }
	m_submapId = 0;
   m_moduleData = NULL;
   m_postalAddress = new PostalAddress();
   m_dashboards = new IntegerArray<UINT32>();
}

/**
 * Destructor
 */
NetObj::~NetObj()
{
   MutexDestroy(m_mutexProperties);
   MutexDestroy(m_mutexRefCount);
   MutexDestroy(m_mutexACL);
   RWLockDestroy(m_rwlockParentList);
   RWLockDestroy(m_rwlockChildList);
   delete m_childList;
   delete m_parentList;
   delete m_accessList;
	safe_free(m_pdwTrustedNodes);
   safe_free(m_comments);
   delete m_moduleData;
   delete m_postalAddress;
   delete m_dashboards;
}

/**
 * Get class name for this object
 */
const TCHAR *NetObj::getObjectClassName() const
{
   int c = getObjectClass();
   return ((c >= 0) && (c < sizeof(s_className) / sizeof(const TCHAR *))) ? s_className[c] : _T("Custom");
}

/**
 * Get class name for given class ID
 */
const TCHAR *NetObj::getObjectClassName(int objectClass)
{
   return ((objectClass >= 0) && (objectClass < sizeof(s_className) / sizeof(const TCHAR *))) ? s_className[objectClass] : _T("Custom");
}

/**
 * Create object from database data
 */
bool NetObj::loadFromDatabase(DB_HANDLE hdb, UINT32 dwId)
{
   return false;     // Abstract objects cannot be loaded from database
}

/**
 * Link related objects after loading from database
 */
void NetObj::linkObjects()
{
}

/**
 * Save object to database
 */
BOOL NetObj::saveToDatabase(DB_HANDLE hdb)
{
   return FALSE;     // Abstract objects cannot be saved to database
}

/**
 * Parameters for DeleteModuleDataCallback and SaveModuleDataCallback
 */
struct ModuleDataDatabaseCallbackParams
{
   UINT32 id;
   DB_HANDLE hdb;
};

/**
 * Callback for deleting module data from database
 */
static EnumerationCallbackResult DeleteModuleDataCallback(const TCHAR *key, const void *value, void *data)
{
   return ((ModuleData *)value)->deleteFromDatabase(((ModuleDataDatabaseCallbackParams *)data)->hdb, ((ModuleDataDatabaseCallbackParams *)data)->id) ? _CONTINUE : _STOP;
}

/**
 * Delete object from database
 */
bool NetObj::deleteFromDatabase(DB_HANDLE hdb)
{
   // Delete ACL
   bool success = executeQueryOnObject(hdb, _T("DELETE FROM acl WHERE object_id=?"));
   if (success)
      success = executeQueryOnObject(hdb, _T("DELETE FROM object_properties WHERE object_id=?"));
   if (success)
      success = executeQueryOnObject(hdb, _T("DELETE FROM object_custom_attributes WHERE object_id=?"));

   // Delete events
   if (success && ConfigReadInt(_T("DeleteEventsOfDeletedObject"), 1))
   {
      success = executeQueryOnObject(hdb, _T("DELETE FROM event_log WHERE event_source=?"));
   }

   // Delete alarms
   if (success && ConfigReadInt(_T("DeleteAlarmsOfDeletedObject"), 1))
   {
      success = DeleteObjectAlarms(m_id, hdb);
   }

   if (success && isLocationTableExists(hdb))
   {
      TCHAR query[256];
      _sntprintf(query, 256, _T("DROP TABLE gps_history_%d"), m_id);
      success = DBQuery(hdb, query);
   }

   // Delete module data
   if (success && (m_moduleData != NULL))
   {
      ModuleDataDatabaseCallbackParams data;
      data.id = m_id;
      data.hdb = hdb;
      success = (m_moduleData->forEach(DeleteModuleDataCallback, &data) == _CONTINUE);
   }

   return success;
}

/**
 * Load common object properties from database
 */
bool NetObj::loadCommonProperties(DB_HANDLE hdb)
{
   bool success = false;

   // Load access options
	DB_STATEMENT hStmt = DBPrepare(hdb,
	                          _T("SELECT name,status,is_deleted,")
                             _T("inherit_access_rights,last_modified,status_calc_alg,")
                             _T("status_prop_alg,status_fixed_val,status_shift,")
                             _T("status_translation,status_single_threshold,")
                             _T("status_thresholds,comments,is_system,")
									  _T("location_type,latitude,longitude,location_accuracy,")
									  _T("location_timestamp,guid,image,submap_id,country,city,")
                             _T("street_address,postcode,maint_mode,maint_event_id FROM object_properties ")
                             _T("WHERE object_id=?"));
	if (hStmt != NULL)
	{
		DBBind(hStmt, 1, DB_SQLTYPE_INTEGER, m_id);
		DB_RESULT hResult = DBSelectPrepared(hStmt);
		if (hResult != NULL)
		{
			if (DBGetNumRows(hResult) > 0)
			{
				DBGetField(hResult, 0, 0, m_name, MAX_OBJECT_NAME);
				m_status = DBGetFieldLong(hResult, 0, 1);
				m_isDeleted = DBGetFieldLong(hResult, 0, 2) ? true : false;
				m_inheritAccessRights = DBGetFieldLong(hResult, 0, 3) ? true : false;
				m_dwTimeStamp = DBGetFieldULong(hResult, 0, 4);
				m_statusCalcAlg = DBGetFieldLong(hResult, 0, 5);
				m_statusPropAlg = DBGetFieldLong(hResult, 0, 6);
				m_fixedStatus = DBGetFieldLong(hResult, 0, 7);
				m_statusShift = DBGetFieldLong(hResult, 0, 8);
				DBGetFieldByteArray(hResult, 0, 9, m_statusTranslation, 4, STATUS_WARNING);
				m_statusSingleThreshold = DBGetFieldLong(hResult, 0, 10);
				DBGetFieldByteArray(hResult, 0, 11, m_statusThresholds, 4, 50);
				safe_free(m_comments);
				m_comments = DBGetField(hResult, 0, 12, NULL, 0);
				m_isSystem = DBGetFieldLong(hResult, 0, 13) ? true : false;

				int locType = DBGetFieldLong(hResult, 0, 14);
				if (locType != GL_UNSET)
				{
					TCHAR lat[32], lon[32];

					DBGetField(hResult, 0, 15, lat, 32);
					DBGetField(hResult, 0, 16, lon, 32);
					m_geoLocation = GeoLocation(locType, lat, lon, DBGetFieldLong(hResult, 0, 17), DBGetFieldULong(hResult, 0, 18));
				}
				else
				{
					m_geoLocation = GeoLocation();
				}

				m_guid = DBGetFieldGUID(hResult, 0, 19);
				m_image = DBGetFieldGUID(hResult, 0, 20);
				m_submapId = DBGetFieldULong(hResult, 0, 21);

            TCHAR country[64], city[64], streetAddress[256], postcode[32];
            DBGetField(hResult, 0, 22, country, 64);
            DBGetField(hResult, 0, 23, city, 64);
            DBGetField(hResult, 0, 24, streetAddress, 256);
            DBGetField(hResult, 0, 25, postcode, 32);
            delete m_postalAddress;
            m_postalAddress = new PostalAddress(country, city, streetAddress, postcode);

            m_maintenanceMode = DBGetFieldLong(hResult, 0, 26) ? true : false;
            m_maintenanceEventId = DBGetFieldUInt64(hResult, 0, 27);

				success = true;
			}
			DBFreeResult(hResult);
		}
		DBFreeStatement(hStmt);
	}

	// Load custom attributes
	if (success)
	{
		hStmt = DBPrepare(hdb, _T("SELECT attr_name,attr_value FROM object_custom_attributes WHERE object_id=?"));
		if (hStmt != NULL)
		{
			DBBind(hStmt, 1, DB_SQLTYPE_INTEGER, m_id);
			DB_RESULT hResult = DBSelectPrepared(hStmt);
			if (hResult != NULL)
			{
				int i, count;
				TCHAR *name, *value;

				count = DBGetNumRows(hResult);
				for(i = 0; i < count; i++)
				{
		   		name = DBGetField(hResult, i, 0, NULL, 0);
		   		if (name != NULL)
		   		{
						value = DBGetField(hResult, i, 1, NULL, 0);
						if (value != NULL)
						{
							m_customAttributes.setPreallocated(name, value);
						}
					}
				}
				DBFreeResult(hResult);
			}
			else
			{
				success = false;
			}
			DBFreeStatement(hStmt);
		}
		else
		{
			success = false;
		}
	}

   // Load associated dashboards
   if (success)
   {
      hStmt = DBPrepare(hdb, _T("SELECT dashboard_id FROM dashboard_associations WHERE object_id=?"));
      if (hStmt != NULL)
      {
         DBBind(hStmt, 1, DB_SQLTYPE_INTEGER, m_id);
         DB_RESULT hResult = DBSelectPrepared(hStmt);
         if (hResult != NULL)
         {
            int count = DBGetNumRows(hResult);
            for(int i = 0; i < count; i++)
            {
               m_dashboards->add(DBGetFieldULong(hResult, i, 0));
            }
            DBFreeResult(hResult);
         }
         else
         {
            success = false;
         }
         DBFreeStatement(hStmt);
      }
      else
      {
         success = false;
      }
   }

	if (success)
		success = loadTrustedNodes(hdb);

	if (!success)
		DbgPrintf(4, _T("NetObj::loadCommonProperties() failed for object %s [%ld] class=%d"), m_name, (long)m_id, getObjectClass());

   return success;
}

/**
 * Callback for saving custom attribute in database
 */
static EnumerationCallbackResult SaveAttributeCallback(const TCHAR *key, const void *value, void *data)
{
   DB_STATEMENT hStmt = (DB_STATEMENT)data;
   DBBind(hStmt, 2, DB_SQLTYPE_VARCHAR, key, DB_BIND_STATIC);
   DBBind(hStmt, 3, DB_SQLTYPE_VARCHAR, (const TCHAR *)value, DB_BIND_STATIC);
   return DBExecute(hStmt) ? _CONTINUE : _STOP;
}

/**
 * Callback for saving module data in database
 */
static EnumerationCallbackResult SaveModuleDataCallback(const TCHAR *key, const void *value, void *data)
{
   return ((ModuleData *)value)->saveToDatabase(((ModuleDataDatabaseCallbackParams *)data)->hdb, ((ModuleDataDatabaseCallbackParams *)data)->id) ? _CONTINUE : _STOP;
}

/**
 * Save common object properties to database
 */
bool NetObj::saveCommonProperties(DB_HANDLE hdb)
{
	DB_STATEMENT hStmt;
	if (IsDatabaseRecordExist(hdb, _T("object_properties"), _T("object_id"), m_id))
	{
		hStmt = DBPrepare(hdb,
                    _T("UPDATE object_properties SET name=?,status=?,")
                    _T("is_deleted=?,inherit_access_rights=?,")
                    _T("last_modified=?,status_calc_alg=?,status_prop_alg=?,")
                    _T("status_fixed_val=?,status_shift=?,status_translation=?,")
                    _T("status_single_threshold=?,status_thresholds=?,")
                    _T("comments=?,is_system=?,location_type=?,latitude=?,")
						  _T("longitude=?,location_accuracy=?,location_timestamp=?,")
						  _T("guid=?,image=?,submap_id=?,country=?,city=?,")
                    _T("street_address=?,postcode=?,maint_mode=?,maint_event_id=? WHERE object_id=?"));
	}
	else
	{
		hStmt = DBPrepare(hdb,
                    _T("INSERT INTO object_properties (name,status,is_deleted,")
                    _T("inherit_access_rights,last_modified,status_calc_alg,")
                    _T("status_prop_alg,status_fixed_val,status_shift,status_translation,")
                    _T("status_single_threshold,status_thresholds,comments,is_system,")
						  _T("location_type,latitude,longitude,location_accuracy,location_timestamp,")
						  _T("guid,image,submap_id,country,city,street_address,postcode,maint_mode,")
						  _T("maint_event_id,object_id) ")
                    _T("VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
	}
	if (hStmt == NULL)
		return false;

   TCHAR szTranslation[16], szThresholds[16], lat[32], lon[32];
   for(int i = 0, j = 0; i < 4; i++, j += 2)
   {
      _sntprintf(&szTranslation[j], 16 - j, _T("%02X"), (BYTE)m_statusTranslation[i]);
      _sntprintf(&szThresholds[j], 16 - j, _T("%02X"), (BYTE)m_statusThresholds[i]);
   }
	_sntprintf(lat, 32, _T("%f"), m_geoLocation.getLatitude());
	_sntprintf(lon, 32, _T("%f"), m_geoLocation.getLongitude());

	DBBind(hStmt, 1, DB_SQLTYPE_VARCHAR, m_name, DB_BIND_STATIC);
	DBBind(hStmt, 2, DB_SQLTYPE_INTEGER, (LONG)m_status);
   DBBind(hStmt, 3, DB_SQLTYPE_INTEGER, (LONG)(m_isDeleted ? 1 : 0));
	DBBind(hStmt, 4, DB_SQLTYPE_INTEGER, (LONG)(m_inheritAccessRights ? 1 : 0));
	DBBind(hStmt, 5, DB_SQLTYPE_INTEGER, (LONG)m_dwTimeStamp);
	DBBind(hStmt, 6, DB_SQLTYPE_INTEGER, (LONG)m_statusCalcAlg);
	DBBind(hStmt, 7, DB_SQLTYPE_INTEGER, (LONG)m_statusPropAlg);
	DBBind(hStmt, 8, DB_SQLTYPE_INTEGER, (LONG)m_fixedStatus);
	DBBind(hStmt, 9, DB_SQLTYPE_INTEGER, (LONG)m_statusShift);
	DBBind(hStmt, 10, DB_SQLTYPE_VARCHAR, szTranslation, DB_BIND_STATIC);
	DBBind(hStmt, 11, DB_SQLTYPE_INTEGER, (LONG)m_statusSingleThreshold);
	DBBind(hStmt, 12, DB_SQLTYPE_VARCHAR, szThresholds, DB_BIND_STATIC);
	DBBind(hStmt, 13, DB_SQLTYPE_VARCHAR, m_comments, DB_BIND_STATIC);
   DBBind(hStmt, 14, DB_SQLTYPE_INTEGER, (LONG)(m_isSystem ? 1 : 0));
	DBBind(hStmt, 15, DB_SQLTYPE_INTEGER, (LONG)m_geoLocation.getType());
	DBBind(hStmt, 16, DB_SQLTYPE_VARCHAR, lat, DB_BIND_STATIC);
	DBBind(hStmt, 17, DB_SQLTYPE_VARCHAR, lon, DB_BIND_STATIC);
	DBBind(hStmt, 18, DB_SQLTYPE_INTEGER, (LONG)m_geoLocation.getAccuracy());
	DBBind(hStmt, 19, DB_SQLTYPE_INTEGER, (UINT32)m_geoLocation.getTimestamp());
	DBBind(hStmt, 20, DB_SQLTYPE_VARCHAR, m_guid);
	DBBind(hStmt, 21, DB_SQLTYPE_VARCHAR, m_image);
	DBBind(hStmt, 22, DB_SQLTYPE_INTEGER, m_submapId);
	DBBind(hStmt, 23, DB_SQLTYPE_VARCHAR, m_postalAddress->getCountry(), DB_BIND_STATIC);
	DBBind(hStmt, 24, DB_SQLTYPE_VARCHAR, m_postalAddress->getCity(), DB_BIND_STATIC);
	DBBind(hStmt, 25, DB_SQLTYPE_VARCHAR, m_postalAddress->getStreetAddress(), DB_BIND_STATIC);
	DBBind(hStmt, 26, DB_SQLTYPE_VARCHAR, m_postalAddress->getPostCode(), DB_BIND_STATIC);
   DBBind(hStmt, 27, DB_SQLTYPE_VARCHAR, m_maintenanceMode ? _T("1") : _T("0"), DB_BIND_STATIC);
   DBBind(hStmt, 28, DB_SQLTYPE_BIGINT, m_maintenanceEventId);
	DBBind(hStmt, 29, DB_SQLTYPE_INTEGER, m_id);

   bool success = DBExecute(hStmt);
	DBFreeStatement(hStmt);

   // Save custom attributes
   if (success)
   {
		TCHAR szQuery[512];
		_sntprintf(szQuery, 512, _T("DELETE FROM object_custom_attributes WHERE object_id=%d"), m_id);
      success = DBQuery(hdb, szQuery);
		if (success)
		{
			hStmt = DBPrepare(hdb, _T("INSERT INTO object_custom_attributes (object_id,attr_name,attr_value) VALUES (?,?,?)"));
			if (hStmt != NULL)
			{
				DBBind(hStmt, 1, DB_SQLTYPE_INTEGER, m_id);
            success = (m_customAttributes.forEach(SaveAttributeCallback, hStmt) == _CONTINUE);
				DBFreeStatement(hStmt);
			}
			else
			{
				success = false;
			}
		}
   }

   // Save dashboard associations
   if (success)
   {
      TCHAR szQuery[512];
      _sntprintf(szQuery, 512, _T("DELETE FROM dashboard_associations WHERE object_id=%d"), m_id);
      success = DBQuery(hdb, szQuery);
      if (success && (m_dashboards->size() > 0))
      {
         hStmt = DBPrepare(hdb, _T("INSERT INTO dashboard_associations (object_id,dashboard_id) VALUES (?,?)"));
         if (hStmt != NULL)
         {
            DBBind(hStmt, 1, DB_SQLTYPE_INTEGER, m_id);
            for(int i = 0; (i < m_dashboards->size()) && success; i++)
            {
               DBBind(hStmt, 2, DB_SQLTYPE_INTEGER, m_dashboards->get(i));
               success = DBExecute(hStmt);
            }
            DBFreeStatement(hStmt);
         }
         else
         {
            success = false;
         }
      }
   }

   // Save module data
   if (success && (m_moduleData != NULL))
   {
      ModuleDataDatabaseCallbackParams data;
      data.id = m_id;
      data.hdb = hdb;
      success = (m_moduleData->forEach(SaveModuleDataCallback, &data) == _CONTINUE);
   }

	if (success)
		success = saveTrustedNodes(hdb);

   return success;
}

/**
 * Add reference to the new child object
 */
void NetObj::addChild(NetObj *object)
{
   lockChildList(true);
   if (m_childList->contains(object))
   {
      unlockChildList();
      return;     // Already in the child list
   }
   m_childList->add(object);
   unlockChildList();
	incRefCount();
   setModified();
   DbgPrintf(7, _T("NetObj::addChild: this=%s [%d]; object=%s [%d]"), m_name, m_id, object->m_name, object->m_id);
}

/**
 * Add reference to parent object
 */
void NetObj::addParent(NetObj *object)
{
   lockParentList(true);
   if (m_parentList->contains(object))
   {
      unlockParentList();
      return;     // Already in the parents list
   }
   m_parentList->add(object);
   unlockParentList();
	incRefCount();
   setModified();
   DbgPrintf(7, _T("NetObj::addParent: this=%s [%d]; object=%s [%d]"), m_name, m_id, object->m_name, object->m_id);
}

/**
 * Delete reference to child object
 */
void NetObj::deleteChild(NetObj *object)
{
   int i;

   lockChildList(true);
   for(i = 0; i < m_childList->size(); i++)
      if (m_childList->get(i) == object)
         break;

   if (i == m_childList->size())   // No such object
   {
      unlockChildList();
      return;
   }

   DbgPrintf(7, _T("NetObj::deleteChild: this=%s [%d]; object=%s [%d]"), m_name, m_id, object->m_name, object->m_id);
   m_childList->remove(i);
   unlockChildList();
	decRefCount();
   setModified();
}

/**
 * Delete reference to parent object
 */
void NetObj::deleteParent(NetObj *object)
{
   int i;

   lockParentList(true);
   for(i = 0; i < m_parentList->size(); i++)
      if (m_parentList->get(i) == object)
         break;
   if (i == m_parentList->size())   // No such object
   {
      unlockParentList();
      return;
   }

   DbgPrintf(7, _T("NetObj::deleteParent: this=%s [%d]; object=%s [%d]"), m_name, m_id, object->m_name, object->m_id);

   m_parentList->remove(i);
   unlockParentList();
	decRefCount();
   setModified();
}

/**
 * Walker callback to call OnObjectDelete for each active object
 */
void NetObj::onObjectDeleteCallback(NetObj *object, void *data)
{
	UINT32 currId = ((NetObj *)data)->getId();
	if ((object->getId() != currId) && !object->isDeleted())
		object->onObjectDelete(currId);
}

/**
 * Prepare object for deletion - remove all references, etc.
 *
 * @param initiator pointer to parent object which causes recursive deletion or NULL
 */
void NetObj::deleteObject(NetObj *initiator)
{
   DbgPrintf(4, _T("Deleting object %d [%s]"), m_id, m_name);

	// Prevent object change propagation until it's marked as deleted
	// (to prevent the object's incorrect appearance in GUI
	lockProperties();
   m_isHidden = true;
	unlockProperties();

	// Notify modules about object deletion
   CALL_ALL_MODULES(pfPreObjectDelete, (this));

   prepareForDeletion();

   DbgPrintf(5, _T("NetObj::deleteObject(): deleting object %d from indexes"), m_id);
   NetObjDeleteFromIndexes(this);

   // Delete references to this object from child objects
   DbgPrintf(5, _T("NetObj::deleteObject(): clearing child list for object %d"), m_id);
   ObjectArray<NetObj> *deleteList = NULL;
   lockChildList(true);
   for(int i = 0; i < m_childList->size(); i++)
   {
      NetObj *o = m_childList->get(i);
      if (o->getParentCount() == 1)
      {
         // last parent, delete object
         if (deleteList == NULL)
            deleteList = new ObjectArray<NetObj>(16, 16, false);
			deleteList->add(o);
      }
      else
      {
         o->deleteParent(this);
      }
		decRefCount();
   }
   m_childList->clear();
   unlockChildList();

   // Remove references to this object from parent objects
   DbgPrintf(5, _T("NetObj::Delete(): clearing parent list for object %d"), m_id);
   lockParentList(true);
   for(int i = 0; i < m_parentList->size(); i++)
   {
      // If parent is deletion initiator then this object already
      // removed from parent's list
      NetObj *obj = m_parentList->get(i);
      if (obj != initiator)
      {
         obj->deleteChild(this);
         if ((obj->getObjectClass() == OBJECT_SUBNET) && (g_flags & AF_DELETE_EMPTY_SUBNETS) && (obj->getChildCount() == 0))
         {
            if (deleteList == NULL)
               deleteList = new ObjectArray<NetObj>(16, 16, false);
            deleteList->add(obj);
         }
         else
            obj->calculateCompoundStatus();
      }
		decRefCount();
   }
   m_parentList->clear();
   unlockParentList();

   // Delete orphaned child objects and empty subnets
   if (deleteList != NULL)
   {
      for(int i = 0; i < deleteList->size(); i++)
      {
         NetObj *obj = deleteList->get(i);
         DbgPrintf(5, _T("NetObj::deleteObject(): calling deleteObject() on %s [%d]"), obj->getName(), obj->getId());
         obj->deleteObject(this);
      }
      delete deleteList;
   }

   lockProperties();
   m_isHidden = false;
   m_isDeleted = true;
   setModified();
   unlockProperties();

   // Notify all other objects about object deletion
   DbgPrintf(5, _T("NetObj::deleteObject(): calling onObjectDelete(%d)"), m_id);
	g_idxObjectById.forEach(onObjectDeleteCallback, this);

   DbgPrintf(4, _T("Object %d successfully deleted"), m_id);
}

/**
 * Default handler for object deletion notification
 */
void NetObj::onObjectDelete(UINT32 dwObjectId)
{
}

/**
 * Get childs IDs in printable form
 */
const TCHAR *NetObj::dbgGetChildList(TCHAR *szBuffer)
{
   TCHAR *pBuf = szBuffer;
   *pBuf = 0;
   lockChildList(false);
   for(int i = 0; i < m_childList->size(); i++)
   {
      _sntprintf(pBuf, 10, _T("%d "), m_childList->get(i)->getId());
      while(*pBuf)
         pBuf++;
   }
   unlockChildList();
   if (pBuf != szBuffer)
      *(pBuf - 1) = 0;
   return szBuffer;
}

/**
 * Get parents IDs in printable form
 */
const TCHAR *NetObj::dbgGetParentList(TCHAR *szBuffer)
{
   TCHAR *pBuf = szBuffer;
   *pBuf = 0;
   lockParentList(false);
   for(int i = 0; i < m_parentList->size(); i++)
   {
      _sntprintf(pBuf, 10, _T("%d "), m_parentList->get(i)->getId());
      while(*pBuf)
         pBuf++;
   }
   unlockParentList();
   if (pBuf != szBuffer)
      *(pBuf - 1) = 0;
   return szBuffer;
}

/**
 * Calculate status for compound object based on childs' status
 */
void NetObj::calculateCompoundStatus(BOOL bForcedRecalc)
{
   if (m_status == STATUS_UNMANAGED)
      return;

   int mostCriticalAlarm = GetMostCriticalStatusForObject(m_id);
   int mostCriticalDCI =
      (getObjectClass() == OBJECT_NODE || getObjectClass() == OBJECT_MOBILEDEVICE || getObjectClass() == OBJECT_CLUSTER || getObjectClass() == OBJECT_ACCESSPOINT) ?
         ((DataCollectionTarget *)this)->getMostCriticalDCIStatus() : STATUS_UNKNOWN;

   int oldStatus = m_status;
   int mostCriticalStatus, i, count, iStatusAlg;
   int nSingleThreshold, *pnThresholds;
   int nRating[5], iChildStatus, nThresholds[4];

   lockProperties();
   if (m_statusCalcAlg == SA_CALCULATE_DEFAULT)
   {
      iStatusAlg = GetDefaultStatusCalculation(&nSingleThreshold, &pnThresholds);
   }
   else
   {
      iStatusAlg = m_statusCalcAlg;
      nSingleThreshold = m_statusSingleThreshold;
      pnThresholds = m_statusThresholds;
   }
   if (iStatusAlg == SA_CALCULATE_SINGLE_THRESHOLD)
   {
      for(i = 0; i < 4; i++)
         nThresholds[i] = nSingleThreshold;
      pnThresholds = nThresholds;
   }

   switch(iStatusAlg)
   {
      case SA_CALCULATE_MOST_CRITICAL:
         lockChildList(false);
         for(i = 0, count = 0, mostCriticalStatus = -1; i < m_childList->size(); i++)
         {
            iChildStatus = m_childList->get(i)->getPropagatedStatus();
            if ((iChildStatus < STATUS_UNKNOWN) &&
                (iChildStatus > mostCriticalStatus))
            {
               mostCriticalStatus = iChildStatus;
               count++;
            }
         }
         m_status = (count > 0) ? mostCriticalStatus : STATUS_UNKNOWN;
         unlockChildList();
         break;
      case SA_CALCULATE_SINGLE_THRESHOLD:
      case SA_CALCULATE_MULTIPLE_THRESHOLDS:
         // Step 1: calculate severity raitings
         memset(nRating, 0, sizeof(int) * 5);
         lockChildList(false);
         for(i = 0, count = 0; i < m_childList->size(); i++)
         {
            iChildStatus = m_childList->get(i)->getPropagatedStatus();
            if (iChildStatus < STATUS_UNKNOWN)
            {
               while(iChildStatus >= 0)
                  nRating[iChildStatus--]++;
               count++;
            }
         }
         unlockChildList();

         // Step 2: check what severity rating is above threshold
         if (count > 0)
         {
            for(i = 4; i > 0; i--)
               if (nRating[i] * 100 / count >= pnThresholds[i - 1])
                  break;
            m_status = i;
         }
         else
         {
            m_status = STATUS_UNKNOWN;
         }
         break;
      default:
         m_status = STATUS_UNKNOWN;
         break;
   }

   // If alarms exist for object, apply alarm severity to object's status
   if (mostCriticalAlarm != STATUS_UNKNOWN)
   {
      if (m_status == STATUS_UNKNOWN)
      {
         m_status = mostCriticalAlarm;
      }
      else
      {
         m_status = MAX(m_status, mostCriticalAlarm);
      }
   }

   // If DCI status is calculated for object apply DCI object's statud
   if (mostCriticalDCI != STATUS_UNKNOWN)
   {
      if (m_status == STATUS_UNKNOWN)
      {
         m_status = mostCriticalDCI;
      }
      else
      {
         m_status = MAX(m_status, mostCriticalDCI);
      }
   }

   // Query loaded modules for object status
   ENUMERATE_MODULES(pfCalculateObjectStatus)
   {
      int moduleStatus = g_pModuleList[__i].pfCalculateObjectStatus(this);
      if (moduleStatus != STATUS_UNKNOWN)
      {
         if (m_status == STATUS_UNKNOWN)
         {
            m_status = moduleStatus;
         }
         else
         {
            m_status = MAX(m_status, moduleStatus);
         }
      }
   }

   unlockProperties();

   // Cause parent object(s) to recalculate it's status
   if ((oldStatus != m_status) || bForcedRecalc)
   {
      lockParentList(false);
      for(i = 0; i < m_parentList->size(); i++)
         m_parentList->get(i)->calculateCompoundStatus();
      unlockParentList();
      lockProperties();
      setModified();
      unlockProperties();
   }
}

/**
 * Load ACL from database
 */
bool NetObj::loadACLFromDB(DB_HANDLE hdb)
{
   bool success = false;

	DB_STATEMENT hStmt = DBPrepare(hdb, _T("SELECT user_id,access_rights FROM acl WHERE object_id=?"));
	if (hStmt != NULL)
	{
		DBBind(hStmt, 1, DB_SQLTYPE_INTEGER, m_id);
		DB_RESULT hResult = DBSelectPrepared(hStmt);
		if (hResult != NULL)
		{
			int count = DBGetNumRows(hResult);
			for(int i = 0; i < count; i++)
				m_accessList->addElement(DBGetFieldULong(hResult, i, 0), DBGetFieldULong(hResult, i, 1));
			DBFreeResult(hResult);
			success = true;
		}
		DBFreeStatement(hStmt);
	}
   return success;
}

/**
 * ACL enumeration parameters structure
 */
struct SAVE_PARAM
{
   DB_HANDLE hdb;
   UINT32 dwObjectId;
};

/**
 * Handler for ACL elements enumeration
 */
static void EnumerationHandler(UINT32 dwUserId, UINT32 dwAccessRights, void *pArg)
{
   TCHAR szQuery[256];

   _sntprintf(szQuery, sizeof(szQuery) / sizeof(TCHAR), _T("INSERT INTO acl (object_id,user_id,access_rights) VALUES (%d,%d,%d)"),
           ((SAVE_PARAM *)pArg)->dwObjectId, dwUserId, dwAccessRights);
   DBQuery(((SAVE_PARAM *)pArg)->hdb, szQuery);
}

/**
 * Save ACL to database
 */
bool NetObj::saveACLToDB(DB_HANDLE hdb)
{
   TCHAR szQuery[256];
   bool success = false;
   SAVE_PARAM sp;

   // Save access list
   lockACL();
   _sntprintf(szQuery, sizeof(szQuery) / sizeof(TCHAR), _T("DELETE FROM acl WHERE object_id=%d"), m_id);
   if (DBQuery(hdb, szQuery))
   {
      sp.dwObjectId = m_id;
      sp.hdb = hdb;
      m_accessList->enumerateElements(EnumerationHandler, &sp);
      success = true;
   }
   unlockACL();
   return success;
}

/**
 * Data for SendModuleDataCallback
 */
struct SendModuleDataCallbackData
{
   NXCPMessage *msg;
   UINT32 id;
};

/**
 * Callback for sending module data in NXCP message
 */
static EnumerationCallbackResult SendModuleDataCallback(const TCHAR *key, const void *value, void *data)
{
   ((SendModuleDataCallbackData *)data)->msg->setField(((SendModuleDataCallbackData *)data)->id, key);
   ((ModuleData *)value)->fillMessage(((SendModuleDataCallbackData *)data)->msg, ((SendModuleDataCallbackData *)data)->id + 1);
   ((SendModuleDataCallbackData *)data)->id += 0x100000;
   return _CONTINUE;
}

/**
 * Fill NXCP message with object's data
 * Object's properties are locked when this method is called. Method should not do any other locks.
 * Data required other locks should be filled in fillMessageInternalStage2().
 */
void NetObj::fillMessageInternal(NXCPMessage *pMsg)
{
   pMsg->setField(VID_OBJECT_CLASS, (WORD)getObjectClass());
   pMsg->setField(VID_OBJECT_ID, m_id);
	pMsg->setField(VID_GUID, m_guid);
   pMsg->setField(VID_OBJECT_NAME, m_name);
   pMsg->setField(VID_OBJECT_STATUS, (WORD)m_status);
   pMsg->setField(VID_IS_DELETED, (WORD)(m_isDeleted ? 1 : 0));
   pMsg->setField(VID_IS_SYSTEM, (INT16)(m_isSystem ? 1 : 0));
   pMsg->setField(VID_MAINTENANCE_MODE, (INT16)(m_maintenanceEventId ? 1 : 0));

   pMsg->setField(VID_INHERIT_RIGHTS, m_inheritAccessRights);
   pMsg->setField(VID_STATUS_CALCULATION_ALG, (WORD)m_statusCalcAlg);
   pMsg->setField(VID_STATUS_PROPAGATION_ALG, (WORD)m_statusPropAlg);
   pMsg->setField(VID_FIXED_STATUS, (WORD)m_fixedStatus);
   pMsg->setField(VID_STATUS_SHIFT, (WORD)m_statusShift);
   pMsg->setField(VID_STATUS_TRANSLATION_1, (WORD)m_statusTranslation[0]);
   pMsg->setField(VID_STATUS_TRANSLATION_2, (WORD)m_statusTranslation[1]);
   pMsg->setField(VID_STATUS_TRANSLATION_3, (WORD)m_statusTranslation[2]);
   pMsg->setField(VID_STATUS_TRANSLATION_4, (WORD)m_statusTranslation[3]);
   pMsg->setField(VID_STATUS_SINGLE_THRESHOLD, (WORD)m_statusSingleThreshold);
   pMsg->setField(VID_STATUS_THRESHOLD_1, (WORD)m_statusThresholds[0]);
   pMsg->setField(VID_STATUS_THRESHOLD_2, (WORD)m_statusThresholds[1]);
   pMsg->setField(VID_STATUS_THRESHOLD_3, (WORD)m_statusThresholds[2]);
   pMsg->setField(VID_STATUS_THRESHOLD_4, (WORD)m_statusThresholds[3]);
   pMsg->setField(VID_COMMENTS, CHECK_NULL_EX(m_comments));
	pMsg->setField(VID_IMAGE, m_image);
	pMsg->setField(VID_DRILL_DOWN_OBJECT_ID, m_submapId);
	pMsg->setField(VID_NUM_TRUSTED_NODES, m_dwNumTrustedNodes);
	if (m_dwNumTrustedNodes > 0)
		pMsg->setFieldFromInt32Array(VID_TRUSTED_NODES, m_dwNumTrustedNodes, m_pdwTrustedNodes);
   pMsg->setFieldFromInt32Array(VID_DASHBOARDS, m_dashboards);

   m_customAttributes.fillMessage(pMsg, VID_NUM_CUSTOM_ATTRIBUTES, VID_CUSTOM_ATTRIBUTES_BASE);

	m_geoLocation.fillMessage(*pMsg);

   pMsg->setField(VID_COUNTRY, m_postalAddress->getCountry());
   pMsg->setField(VID_CITY, m_postalAddress->getCity());
   pMsg->setField(VID_STREET_ADDRESS, m_postalAddress->getStreetAddress());
   pMsg->setField(VID_POSTCODE, m_postalAddress->getPostCode());

   if (m_moduleData != NULL)
   {
      pMsg->setField(VID_MODULE_DATA_COUNT, (UINT16)m_moduleData->size());
      SendModuleDataCallbackData data;
      data.msg = pMsg;
      data.id = VID_MODULE_DATA_BASE;
      m_moduleData->forEach(SendModuleDataCallback, &data);
   }
   else
   {
      pMsg->setField(VID_MODULE_DATA_COUNT, (UINT16)0);
   }
}

/**
 * Fill NXCP message with object's data - stage 2
 * Object's properties are not locked when this method is called. Should be
 * used only to fill data where properties lock is not enough (like data
 * collection configuration).
 */
void NetObj::fillMessageInternalStage2(NXCPMessage *pMsg)
{
}

/**
 * Fill NXCP message with object's data
 */
void NetObj::fillMessage(NXCPMessage *msg)
{
   lockProperties();
   fillMessageInternal(msg);
   unlockProperties();
   fillMessageInternalStage2(msg);

   lockACL();
   m_accessList->fillMessage(msg);
   unlockACL();

   UINT32 dwId;
   int i;

   lockParentList(false);
   msg->setField(VID_PARENT_CNT, m_parentList->size());
   for(i = 0, dwId = VID_PARENT_ID_BASE; i < m_parentList->size(); i++, dwId++)
      msg->setField(dwId, m_parentList->get(i)->getId());
   unlockParentList();

   lockChildList(false);
   msg->setField(VID_CHILD_CNT, m_childList->size());
   for(i = 0, dwId = VID_CHILD_ID_BASE; i < m_childList->size(); i++, dwId++)
      msg->setField(dwId, m_childList->get(i)->getId());
   unlockChildList();
}

/**
 * Handler for EnumerateSessions()
 */
static void BroadcastObjectChange(ClientSession *pSession, void *pArg)
{
   if (pSession->isAuthenticated())
      pSession->onObjectChange((NetObj *)pArg);
}

/**
 * Mark object as modified and put on client's notification queue
 * We assume that object is locked at the time of function call
 */
void NetObj::setModified(bool notify)
{
   if (g_bModificationsLocked)
      return;

   m_isModified = true;
   m_dwTimeStamp = (UINT32)time(NULL);

   // Send event to all connected clients
   if (notify && !m_isHidden && !m_isSystem)
      EnumerateClientSessions(BroadcastObjectChange, this);
}

/**
 * Modify object from NXCP message - common wrapper
 */
UINT32 NetObj::modifyFromMessage(NXCPMessage *msg)
{
   lockProperties();
   UINT32 rcc = modifyFromMessageInternal(msg);
   setModified();
   unlockProperties();
   return rcc;
}

/**
 * Modify object from NXCP message
 */
UINT32 NetObj::modifyFromMessageInternal(NXCPMessage *pRequest)
{
   // Change object's name
   if (pRequest->isFieldExist(VID_OBJECT_NAME))
      pRequest->getFieldAsString(VID_OBJECT_NAME, m_name, MAX_OBJECT_NAME);

   // Change object's status calculation/propagation algorithms
   if (pRequest->isFieldExist(VID_STATUS_CALCULATION_ALG))
   {
      m_statusCalcAlg = pRequest->getFieldAsInt16(VID_STATUS_CALCULATION_ALG);
      m_statusPropAlg = pRequest->getFieldAsInt16(VID_STATUS_PROPAGATION_ALG);
      m_fixedStatus = pRequest->getFieldAsInt16(VID_FIXED_STATUS);
      m_statusShift = pRequest->getFieldAsInt16(VID_STATUS_SHIFT);
      m_statusTranslation[0] = pRequest->getFieldAsInt16(VID_STATUS_TRANSLATION_1);
      m_statusTranslation[1] = pRequest->getFieldAsInt16(VID_STATUS_TRANSLATION_2);
      m_statusTranslation[2] = pRequest->getFieldAsInt16(VID_STATUS_TRANSLATION_3);
      m_statusTranslation[3] = pRequest->getFieldAsInt16(VID_STATUS_TRANSLATION_4);
      m_statusSingleThreshold = pRequest->getFieldAsInt16(VID_STATUS_SINGLE_THRESHOLD);
      m_statusThresholds[0] = pRequest->getFieldAsInt16(VID_STATUS_THRESHOLD_1);
      m_statusThresholds[1] = pRequest->getFieldAsInt16(VID_STATUS_THRESHOLD_2);
      m_statusThresholds[2] = pRequest->getFieldAsInt16(VID_STATUS_THRESHOLD_3);
      m_statusThresholds[3] = pRequest->getFieldAsInt16(VID_STATUS_THRESHOLD_4);
   }

	// Change image
	if (pRequest->isFieldExist(VID_IMAGE))
		m_image = pRequest->getFieldAsGUID(VID_IMAGE);

   // Change object's ACL
   if (pRequest->isFieldExist(VID_ACL_SIZE))
   {
      lockACL();
      m_inheritAccessRights = pRequest->getFieldAsBoolean(VID_INHERIT_RIGHTS);
      m_accessList->deleteAll();
      int count = pRequest->getFieldAsUInt32(VID_ACL_SIZE);
      for(int i = 0; i < count; i++)
         m_accessList->addElement(pRequest->getFieldAsUInt32(VID_ACL_USER_BASE + i),
                                  pRequest->getFieldAsUInt32(VID_ACL_RIGHTS_BASE + i));
      unlockACL();
   }

	// Change trusted nodes list
   if (pRequest->isFieldExist(VID_NUM_TRUSTED_NODES))
   {
      m_dwNumTrustedNodes = pRequest->getFieldAsUInt32(VID_NUM_TRUSTED_NODES);
		m_pdwTrustedNodes = (UINT32 *)realloc(m_pdwTrustedNodes, sizeof(UINT32) * m_dwNumTrustedNodes);
		pRequest->getFieldAsInt32Array(VID_TRUSTED_NODES, m_dwNumTrustedNodes, m_pdwTrustedNodes);
   }

   // Change custom attributes
   if (pRequest->isFieldExist(VID_NUM_CUSTOM_ATTRIBUTES))
   {
      UINT32 i, dwId, dwNumElements;
      TCHAR *name, *value;

      dwNumElements = pRequest->getFieldAsUInt32(VID_NUM_CUSTOM_ATTRIBUTES);
      m_customAttributes.clear();
      for(i = 0, dwId = VID_CUSTOM_ATTRIBUTES_BASE; i < dwNumElements; i++)
      {
      	name = pRequest->getFieldAsString(dwId++);
      	value = pRequest->getFieldAsString(dwId++);
      	if ((name != NULL) && (value != NULL))
	      	m_customAttributes.setPreallocated(name, value);
      }
   }

	// Change geolocation
	if (pRequest->isFieldExist(VID_GEOLOCATION_TYPE))
	{
		m_geoLocation = GeoLocation(*pRequest);
		addLocationToHistory();
	}

	if (pRequest->isFieldExist(VID_DRILL_DOWN_OBJECT_ID))
	{
		m_submapId = pRequest->getFieldAsUInt32(VID_DRILL_DOWN_OBJECT_ID);
	}

   if (pRequest->isFieldExist(VID_COUNTRY))
   {
      TCHAR buffer[64];
      pRequest->getFieldAsString(VID_COUNTRY, buffer, 64);
      m_postalAddress->setCountry(buffer);
   }

   if (pRequest->isFieldExist(VID_CITY))
   {
      TCHAR buffer[64];
      pRequest->getFieldAsString(VID_CITY, buffer, 64);
      m_postalAddress->setCity(buffer);
   }

   if (pRequest->isFieldExist(VID_STREET_ADDRESS))
   {
      TCHAR buffer[256];
      pRequest->getFieldAsString(VID_STREET_ADDRESS, buffer, 256);
      m_postalAddress->setStreetAddress(buffer);
   }

   if (pRequest->isFieldExist(VID_POSTCODE))
   {
      TCHAR buffer[32];
      pRequest->getFieldAsString(VID_POSTCODE, buffer, 32);
      m_postalAddress->setPostCode(buffer);
   }

   // Change dashboard list
   if (pRequest->isFieldExist(VID_DASHBOARDS))
   {
      pRequest->getFieldAsInt32Array(VID_DASHBOARDS, m_dashboards);
   }

   return RCC_SUCCESS;
}

/**
 * Post-modify hook
 */
void NetObj::postModify()
{
   calculateCompoundStatus(TRUE);
}

/**
 * Get rights to object for specific user
 *
 * @param userId user object ID
 */
UINT32 NetObj::getUserRights(UINT32 userId)
{
   UINT32 dwRights;

   // Admin always has all rights to any object
   if (userId == 0)
      return 0xFFFFFFFF;

	// Non-admin users have no rights to system objects
	if (m_isSystem)
		return 0;

   // Check if have direct right assignment
   lockACL();
   bool hasDirectRights = m_accessList->getUserRights(userId, &dwRights);
   unlockACL();

   if (!hasDirectRights)
   {
      // We don't. If this object inherit rights from parents, get them
      if (m_inheritAccessRights)
      {
         dwRights = 0;
         lockParentList(false);
         for(int i = 0; i < m_parentList->size(); i++)
            dwRights |= m_parentList->get(i)->getUserRights(userId);
         unlockParentList();
      }
   }

   return dwRights;
}

/**
 * Check if given user has specific rights on this object
 *
 * @param userId user object ID
 * @param requiredRights bit mask of requested right
 * @return true if user has all rights specified in requested rights bit mask
 */
BOOL NetObj::checkAccessRights(UINT32 userId, UINT32 requiredRights)
{
   UINT32 effectiveRights = getUserRights(userId);
   return (effectiveRights & requiredRights) == requiredRights;
}

/**
 * Drop all user privileges on current object
 */
void NetObj::dropUserAccess(UINT32 dwUserId)
{
   lockACL();
   bool modified = m_accessList->deleteElement(dwUserId);
   unlockACL();
   if (modified)
   {
      lockProperties();
      setModified();
      unlockProperties();
   }
}

/**
 * Set object's management status
 */
void NetObj::setMgmtStatus(BOOL bIsManaged)
{
   int oldStatus;

   lockProperties();

   if ((bIsManaged && (m_status != STATUS_UNMANAGED)) ||
       ((!bIsManaged) && (m_status == STATUS_UNMANAGED)))
   {
      unlockProperties();
      return;  // Status is already correct
   }

   oldStatus = m_status;
   m_status = (bIsManaged ? STATUS_UNKNOWN : STATUS_UNMANAGED);

   // Generate event if current object is a node
   if (getObjectClass() == OBJECT_NODE)
      PostEvent(bIsManaged ? EVENT_NODE_UNKNOWN : EVENT_NODE_UNMANAGED, m_id, "d", oldStatus);

   setModified();
   unlockProperties();

   // Change status for child objects also
   lockChildList(false);
   for(int i = 0; i < m_childList->size(); i++)
      m_childList->get(i)->setMgmtStatus(bIsManaged);
   unlockChildList();

   // Cause parent object(s) to recalculate it's status
   lockParentList(false);
   for(int i = 0; i < m_parentList->size(); i++)
      m_parentList->get(i)->calculateCompoundStatus();
   unlockParentList();
}

/**
 * Check if given object is an our child (possibly indirect, i.e child of child)
 *
 * @param id object ID to test
 */
bool NetObj::isChild(UINT32 id)
{
   bool bResult = false;

   // Check for our own ID (object ID should never change, so we may not lock object's data)
   if (m_id == id)
      bResult = true;

   // First, walk through our own child list
   if (!bResult)
   {
      lockChildList(false);
      for(int i = 0; i < m_childList->size(); i++)
         if (m_childList->get(i)->getId() == id)
         {
            bResult = true;
            break;
         }
      unlockChildList();
   }

   // If given object is not in child list, check if it is indirect child
   if (!bResult)
   {
      lockChildList(false);
      for(int i = 0; i < m_childList->size(); i++)
         if (m_childList->get(i)->isChild(id))
         {
            bResult = true;
            break;
         }
      unlockChildList();
   }

   return bResult;
}

/**
 * Send message to client, who requests poll, if any
 * This method is used by Node and Interface class objects
 */
void NetObj::sendPollerMsg(UINT32 dwRqId, const TCHAR *pszFormat, ...)
{
   if (m_pollRequestor != NULL)
   {
      va_list args;
      TCHAR szBuffer[1024];

      va_start(args, pszFormat);
      _vsntprintf(szBuffer, 1024, pszFormat, args);
      va_end(args);
      m_pollRequestor->sendPollerMsg(dwRqId, szBuffer);
   }
}

/**
 * Add child node objects (direct and indirect childs) to list
 */
void NetObj::addChildNodesToList(ObjectArray<Node> *nodeList, UINT32 dwUserId)
{
   lockChildList(false);

   // Walk through our own child list
   for(int i = 0; i < m_childList->size(); i++)
   {
      NetObj *object = m_childList->get(i);
      if (object->getObjectClass() == OBJECT_NODE)
      {
         // Check if this node already in the list
			int j;
			for(j = 0; j < nodeList->size(); j++)
				if (nodeList->get(j)->getId() == object->getId())
               break;
         if (j == nodeList->size())
         {
            object->incRefCount();
				nodeList->add((Node *)object);
         }
      }
      else
      {
         if (object->checkAccessRights(dwUserId, OBJECT_ACCESS_READ))
            object->addChildNodesToList(nodeList, dwUserId);
      }
   }

   unlockChildList();
}

/**
 * Add child data collection targets (direct and indirect childs) to list
 */
void NetObj::addChildDCTargetsToList(ObjectArray<DataCollectionTarget> *dctList, UINT32 dwUserId)
{
   lockChildList(false);

   // Walk through our own child list
   for(int i = 0; i < m_childList->size(); i++)
   {
      NetObj *object = m_childList->get(i);
      if (!object->checkAccessRights(dwUserId, OBJECT_ACCESS_READ))
         continue;

      if (object->isDataCollectionTarget())
      {
         // Check if this objects already in the list
			int j;
			for(j = 0; j < dctList->size(); j++)
				if (dctList->get(j)->getId() == object->getId())
               break;
         if (j == dctList->size())
         {
            object->incRefCount();
				dctList->add((DataCollectionTarget *)object);
         }
      }
      object->addChildDCTargetsToList(dctList, dwUserId);
   }

   unlockChildList();
}

/**
 * Hide object and all its childs
 */
void NetObj::hide()
{
   lockChildList(false);
   for(int i = 0; i < m_childList->size(); i++)
      m_childList->get(i)->hide();
   unlockChildList();

	lockProperties();
   m_isHidden = true;
   unlockProperties();
}

/**
 * Unhide object and all its childs
 */
void NetObj::unhide()
{
   lockProperties();
   m_isHidden = false;
   if (!m_isSystem)
      EnumerateClientSessions(BroadcastObjectChange, this);
   unlockProperties();

   lockChildList(false);
   for(int i = 0; i < m_childList->size(); i++)
      m_childList->get(i)->unhide();
   unlockChildList();
}

/**
 * Return status propagated to parent
 */
int NetObj::getPropagatedStatus()
{
   int iStatus;

   if (m_statusPropAlg == SA_PROPAGATE_DEFAULT)
   {
      iStatus = DefaultPropagatedStatus(m_status);
   }
   else
   {
      switch(m_statusPropAlg)
      {
         case SA_PROPAGATE_UNCHANGED:
            iStatus = m_status;
            break;
         case SA_PROPAGATE_FIXED:
            iStatus = ((m_status > STATUS_NORMAL) && (m_status < STATUS_UNKNOWN)) ? m_fixedStatus : m_status;
            break;
         case SA_PROPAGATE_RELATIVE:
            if ((m_status > STATUS_NORMAL) && (m_status < STATUS_UNKNOWN))
            {
               iStatus = m_status + m_statusShift;
               if (iStatus < 0)
                  iStatus = 0;
               if (iStatus > STATUS_CRITICAL)
                  iStatus = STATUS_CRITICAL;
            }
            else
            {
               iStatus = m_status;
            }
            break;
         case SA_PROPAGATE_TRANSLATED:
            if ((m_status > STATUS_NORMAL) && (m_status < STATUS_UNKNOWN))
            {
               iStatus = m_statusTranslation[m_status - 1];
            }
            else
            {
               iStatus = m_status;
            }
            break;
         default:
            iStatus = STATUS_UNKNOWN;
            break;
      }
   }
   return iStatus;
}

/**
 * Prepare object for deletion. Method should return only
 * when object deletion is safe
 */
void NetObj::prepareForDeletion()
{
}

/**
 * Set object's comments.
 * NOTE: pszText should be dynamically allocated or NULL
 */
void NetObj::setComments(TCHAR *text)
{
   lockProperties();
   free(m_comments);
   m_comments = text;
   setModified();
   unlockProperties();
}

/**
 * Copy object's comments to NXCP message
 */
void NetObj::commentsToMessage(NXCPMessage *pMsg)
{
   lockProperties();
   pMsg->setField(VID_COMMENTS, CHECK_NULL_EX(m_comments));
   unlockProperties();
}

/**
 * Load trusted nodes list from database
 */
bool NetObj::loadTrustedNodes(DB_HANDLE hdb)
{
	DB_RESULT hResult;
	TCHAR query[256];
	int i, count;

	_sntprintf(query, 256, _T("SELECT target_node_id FROM trusted_nodes WHERE source_object_id=%d"), m_id);
	hResult = DBSelect(hdb, query);
	if (hResult != NULL)
	{
		count = DBGetNumRows(hResult);
		if (count > 0)
		{
			m_dwNumTrustedNodes = count;
			m_pdwTrustedNodes = (UINT32 *)malloc(sizeof(UINT32) * count);
			for(i = 0; i < count; i++)
			{
				m_pdwTrustedNodes[i] = DBGetFieldULong(hResult, i, 0);
			}
		}
		DBFreeResult(hResult);
	}
	return (hResult != NULL);
}

/**
 * Save list of trusted nodes to database
 */
bool NetObj::saveTrustedNodes(DB_HANDLE hdb)
{
	TCHAR query[256];
	UINT32 i;
	bool rc = false;

	_sntprintf(query, 256, _T("DELETE FROM trusted_nodes WHERE source_object_id=%d"), m_id);
	if (DBQuery(hdb, query))
	{
		for(i = 0; i < m_dwNumTrustedNodes; i++)
		{
			_sntprintf(query, 256, _T("INSERT INTO trusted_nodes (source_object_id,target_node_id) VALUES (%d,%d)"),
			           m_id, m_pdwTrustedNodes[i]);
			if (!DBQuery(hdb, query))
				break;
		}
		if (i == m_dwNumTrustedNodes)
			rc = true;
	}
	return rc;
}

/**
 * Check if given node is in trust list
 * Will always return TRUE if system parameter CheckTrustedNodes set to 0
 */
bool NetObj::isTrustedNode(UINT32 id)
{
	bool rc;

	if (g_flags & AF_CHECK_TRUSTED_NODES)
	{
		UINT32 i;

		lockProperties();
		for(i = 0, rc = false; i < m_dwNumTrustedNodes; i++)
		{
			if (m_pdwTrustedNodes[i] == id)
			{
				rc = true;
				break;
			}
		}
		unlockProperties();
	}
	else
	{
		rc = true;
	}
	return rc;
}

/**
 * Get list of parent objects for NXSL script
 */
NXSL_Array *NetObj::getParentsForNXSL()
{
	NXSL_Array *parents = new NXSL_Array;
	int index = 0;

	lockParentList(false);
	for(int i = 0; i < m_parentList->size(); i++)
	{
	   NetObj *obj = m_parentList->get(i);
		if ((obj->getObjectClass() == OBJECT_CONTAINER) ||
			 (obj->getObjectClass() == OBJECT_SERVICEROOT) ||
			 (obj->getObjectClass() == OBJECT_NETWORK))
		{
			parents->set(index++, new NXSL_Value(new NXSL_Object(&g_nxslNetObjClass, obj)));
		}
	}
	unlockParentList();

	return parents;
}

/**
 * Get list of child objects for NXSL script
 */
NXSL_Array *NetObj::getChildrenForNXSL()
{
	NXSL_Array *children = new NXSL_Array;
	int index = 0;

	lockChildList(false);
	for(int i = 0; i < m_childList->size(); i++)
	{
      children->set(index++, m_childList->get(i)->createNXSLObject());
	}
	unlockChildList();

	return children;
}

/**
 * Get full list of child objects (including both direct and indirect childs)
 */
void NetObj::getFullChildListInternal(ObjectIndex *list, bool eventSourceOnly)
{
	lockChildList(false);
	for(int i = 0; i < m_childList->size(); i++)
	{
      NetObj *obj = m_childList->get(i);
		if (!eventSourceOnly || IsEventSource(obj->getObjectClass()))
			list->put(obj->getId(), obj);
		obj->getFullChildListInternal(list, eventSourceOnly);
	}
	unlockChildList();
}

/**
 * Get full list of child objects (including both direct and indirect childs).
 *  Returned array is dynamically allocated and must be deleted by the caller.
 *
 * @param eventSourceOnly if true, only objects that can be event source will be included
 */
ObjectArray<NetObj> *NetObj::getFullChildList(bool eventSourceOnly, bool updateRefCount)
{
	ObjectIndex list;
	getFullChildListInternal(&list, eventSourceOnly);
	return list.getObjects(updateRefCount);
}

/**
 * Get list of child objects (direct only). Returned array is
 * dynamically allocated and must be deleted by the caller.
 *
 * @param typeFilter Only return objects with class ID equals given value.
 *                   Set to -1 to disable filtering.
 */
ObjectArray<NetObj> *NetObj::getChildList(int typeFilter)
{
	lockChildList(false);
	ObjectArray<NetObj> *list = new ObjectArray<NetObj>((int)m_childList->size(), 16, false);
	for(int i = 0; i < m_childList->size(); i++)
	{
		if ((typeFilter == -1) || (typeFilter == m_childList->get(i)->getObjectClass()))
			list->add(m_childList->get(i));
	}
	unlockChildList();
	return list;
}

/**
 * Get list of parent objects (direct only). Returned array is
 * dynamically allocated and must be deleted by the caller.
 *
 * @param typeFilter Only return objects with class ID equals given value.
 *                   Set to -1 to disable filtering.
 */
ObjectArray<NetObj> *NetObj::getParentList(int typeFilter)
{
    lockParentList(false);
    ObjectArray<NetObj> *list = new ObjectArray<NetObj>(m_parentList->size(), 16, false);
    for(int i = 0; i < m_parentList->size(); i++)
    {
        if ((typeFilter == -1) || (typeFilter == m_parentList->get(i)->getObjectClass()))
            list->add(m_parentList->get(i));
    }
    unlockParentList();
    return list;
}

/**
 * FInd child object by name (with optional class filter)
 */
NetObj *NetObj::findChildObject(const TCHAR *name, int typeFilter)
{
   NetObj *object = NULL;
	lockChildList(false);
	for(int i = 0; i < m_childList->size(); i++)
	{
	   NetObj *o = m_childList->get(i);
      if (((typeFilter == -1) || (typeFilter == o->getObjectClass())) && !_tcsicmp(name, o->getName()))
      {
         object = o;
         break;
      }
	}
	unlockChildList();
	return object;
}

/**
 * Find child node by IP address
 */
Node *NetObj::findChildNode(const InetAddress& addr)
{
   Node *node = NULL;
   lockChildList(false);
   for(int i = 0; i < m_childList->size(); i++)
   {
      NetObj *curr = m_childList->get(i);
      if ((curr->getObjectClass() == OBJECT_NODE) && addr.equals(((Node *)curr)->getIpAddress()))
      {
         node = (Node *)curr;
         break;
      }
   }
   unlockChildList();
   return node;
}

/**
 * Called by client session handler to check if threshold summary should
 * be shown for this object. Default implementation always returns false.
 */
bool NetObj::showThresholdSummary()
{
	return false;
}

/**
 * Must return true if object is a possible event source
 */
bool NetObj::isEventSource()
{
   return false;
}

/**
 * Must return true if object is a possible data collection target
 */
bool NetObj::isDataCollectionTarget()
{
   return false;
}

/**
 * Get module data
 */
ModuleData *NetObj::getModuleData(const TCHAR *module)
{
   lockProperties();
   ModuleData *data = (m_moduleData != NULL) ? m_moduleData->get(module) : NULL;
   unlockProperties();
   return data;
}

/**
 * Set module data
 */
void NetObj::setModuleData(const TCHAR *module, ModuleData *data)
{
   lockProperties();
   if (m_moduleData == NULL)
      m_moduleData = new StringObjectMap<ModuleData>(true);
   m_moduleData->set(module, data);
   unlockProperties();
}

/**
 * Add new location entry
 */
void NetObj::addLocationToHistory()
{
   DB_HANDLE hdb = DBConnectionPoolAcquireConnection();
   bool isSamePlace;
   double latitude = 0;
   double longitude = 0;
   UINT32 accuracy = 0;
   UINT32 startTimestamp = 0;
   DB_RESULT hResult;
   if (!isLocationTableExists(hdb))
   {
      DbgPrintf(4, _T("NetObj::addLocationToHistory: Geolocation history table will be created for object %s [%d]"), m_name, m_id);
      if (!createLocationHistoryTable(hdb))
      {
         DbgPrintf(4, _T("NetObj::addLocationToHistory: Error creating geolocation history table for object %s [%d]"), m_name, m_id);
         return;
      }
   }
	const TCHAR *query;
	switch(g_dbSyntax)
	{
		case DB_SYNTAX_ORACLE:
			query = _T("SELECT * FROM (SELECT latitude,longitude,accuracy,start_timestamp FROM gps_history_%d ORDER BY start_timestamp DESC) WHERE ROWNUM<=1");
			break;
		case DB_SYNTAX_MSSQL:
			query = _T("SELECT TOP 1 latitude,longitude,accuracy,start_timestamp FROM gps_history_%d ORDER BY start_timestamp DESC");
			break;
		case DB_SYNTAX_DB2:
			query = _T("SELECT latitude,longitude,accuracy,start_timestamp FROM gps_history_%d ORDER BY start_timestamp DESC FETCH FIRST 200 ROWS ONLY");
			break;
		default:
			query = _T("SELECT latitude,longitude,accuracy,start_timestamp FROM gps_history_%d ORDER BY start_timestamp DESC LIMIT 1");
			break;
	}
   TCHAR preparedQuery[256];
	_sntprintf(preparedQuery, 256, query, m_id);
	DB_STATEMENT hStmt = DBPrepare(hdb, preparedQuery);

   if (hStmt == NULL)
		goto onFail;

   hResult = DBSelectPrepared(hStmt);
   if (hResult == NULL)
		goto onFail;
   if (DBGetNumRows(hResult) > 0)
   {
      latitude = DBGetFieldDouble(hResult, 0, 0);
      longitude = DBGetFieldDouble(hResult, 0, 1);
      accuracy = DBGetFieldLong(hResult, 0, 2);
      startTimestamp = DBGetFieldULong(hResult, 0, 3);
      isSamePlace = m_geoLocation.sameLocation(latitude, longitude, accuracy);
   }
   else
   {
      isSamePlace = false;
   }
   DBFreeResult(hResult);
   DBFreeStatement(hStmt);

   if (isSamePlace)
   {
      TCHAR query[256];
      _sntprintf(query, 255, _T("UPDATE gps_history_%d SET end_timestamp = ? WHERE start_timestamp =? "), m_id);
      hStmt = DBPrepare(hdb, query);
      if (hStmt == NULL)
         goto onFail;
      DBBind(hStmt, 1, DB_SQLTYPE_INTEGER, (UINT32)m_geoLocation.getTimestamp());
      DBBind(hStmt, 2, DB_SQLTYPE_INTEGER, startTimestamp);
   }
   else
   {
      TCHAR query[256];
      _sntprintf(query, 255, _T("INSERT INTO gps_history_%d (latitude,longitude,")
                       _T("accuracy,start_timestamp,end_timestamp) VALUES (?,?,?,?,?)"), m_id);
      hStmt = DBPrepare(hdb, query);
      if (hStmt == NULL)
         goto onFail;

      TCHAR lat[32], lon[32];
      _sntprintf(lat, 32, _T("%f"), m_geoLocation.getLatitude());
      _sntprintf(lon, 32, _T("%f"), m_geoLocation.getLongitude());

      DBBind(hStmt, 1, DB_SQLTYPE_VARCHAR, lat, DB_BIND_STATIC);
      DBBind(hStmt, 2, DB_SQLTYPE_VARCHAR, lon, DB_BIND_STATIC);
      DBBind(hStmt, 3, DB_SQLTYPE_INTEGER, (LONG)m_geoLocation.getAccuracy());
      DBBind(hStmt, 4, DB_SQLTYPE_INTEGER, (UINT32)m_geoLocation.getTimestamp());
      DBBind(hStmt, 5, DB_SQLTYPE_INTEGER, (UINT32)m_geoLocation.getTimestamp());
	}

   if(!DBExecute(hStmt))
   {
      DbgPrintf(1, _T("NetObj::addLocationToHistory: Failed to add location to history. New: lat %f, lon %f, ac %d, t %d. Old: lat %f, lon %f, ac %d, t %d."),
                m_geoLocation.getLatitude(), m_geoLocation.getLongitude(), m_geoLocation.getAccuracy(), (UINT32)m_geoLocation.getTimestamp(),
                latitude, longitude, accuracy, startTimestamp);
   }
   DBFreeStatement(hStmt);
   DBConnectionPoolReleaseConnection(hdb);
   return;

onFail:
   if(hStmt != NULL)
      DBFreeStatement(hStmt);
   DbgPrintf(4, _T("NetObj::addLocationToHistory(%s [%d]): Failed to add location to history"), m_name, m_id);
   DBConnectionPoolReleaseConnection(hdb);
   return;
}

/**
 * Check if given data table exist
 */
bool NetObj::isLocationTableExists(DB_HANDLE hdb)
{
   TCHAR table[256];
   _sntprintf(table, 256, _T("gps_history_%d"), m_id);
   int rc = DBIsTableExist(hdb, table);
   if (rc == DBIsTableExist_Failure)
   {
      _tprintf(_T("WARNING: call to DBIsTableExist(\"%s\") failed\n"), table);
   }
   return rc != DBIsTableExist_NotFound;
}

/**
 * Create table for storing geolocation history for this object
 */
bool NetObj::createLocationHistoryTable(DB_HANDLE hdb)
{
   TCHAR szQuery[256], szQueryTemplate[256];
   MetaDataReadStr(_T("LocationHistory"), szQueryTemplate, 255, _T(""));
   _sntprintf(szQuery, 256, szQueryTemplate, m_id);
   if (!DBQuery(hdb, szQuery))
		return false;

   return true;
}

/**
 * Set status calculation method
 */
void NetObj::setStatusCalculation(int method, int arg1, int arg2, int arg3, int arg4)
{
   lockProperties();
   m_statusCalcAlg = method;
   switch(method)
   {
      case SA_CALCULATE_SINGLE_THRESHOLD:
         m_statusSingleThreshold = arg1;
         break;
      case SA_CALCULATE_MULTIPLE_THRESHOLDS:
         m_statusThresholds[0] = arg1;
         m_statusThresholds[1] = arg2;
         m_statusThresholds[2] = arg3;
         m_statusThresholds[3] = arg4;
         break;
      default:
         break;
   }
   setModified();
   unlockProperties();
}

/**
 * Set status propagation method
 */
void NetObj::setStatusPropagation(int method, int arg1, int arg2, int arg3, int arg4)
{
   lockProperties();
   m_statusPropAlg = method;
   switch(method)
   {
      case SA_PROPAGATE_FIXED:
         m_fixedStatus = arg1;
         break;
      case SA_PROPAGATE_RELATIVE:
         m_statusShift = arg1;
         break;
      case SA_PROPAGATE_TRANSLATED:
         m_statusTranslation[0] = arg1;
         m_statusTranslation[1] = arg2;
         m_statusTranslation[2] = arg3;
         m_statusTranslation[3] = arg4;
         break;
      default:
         break;
   }
   setModified();
   unlockProperties();
}

/**
 * Enter maintenance mode
 */
void NetObj::enterMaintenanceMode()
{
   DbgPrintf(4, _T("Entering maintenance mode for object %s [%d] (%s)"), m_name, m_id, getObjectClassName());

   lockChildList(false);
   for(int i = 0; i < m_childList->size(); i++)
   {
      NetObj *object = m_childList->get(i);
      if (object->getStatus() != STATUS_UNMANAGED)
         object->enterMaintenanceMode();
   }
   unlockChildList();
}

/**
 * Leave maintenance mode
 */
void NetObj::leaveMaintenanceMode()
{
   DbgPrintf(4, _T("Leaving maintenance mode for object %s [%d] (%s)"), m_name, m_id, getObjectClassName());

   lockChildList(false);
   for(int i = 0; i < m_childList->size(); i++)
   {
      NetObj *object = m_childList->get(i);
      if (object->getStatus() != STATUS_UNMANAGED)
         object->leaveMaintenanceMode();
   }
   unlockChildList();
}

/**
 * Get all custom attributes as NXSL hash map
 */
NXSL_Value *NetObj::getCustomAttributesForNXSL() const
{
   NXSL_HashMap *map = new NXSL_HashMap();
   lockProperties();
   StructArray<KeyValuePair> *attributes = m_customAttributes.toArray();
   for(int i = 0; i < attributes->size(); i++)
   {
      KeyValuePair *p = attributes->get(i);
      map->set(p->key, new NXSL_Value((const TCHAR *)p->value));
   }
   unlockProperties();
   delete attributes;
   return new NXSL_Value(map);
}

/**
 * Create NXSL object for this object
 */
NXSL_Value *NetObj::createNXSLObject()
{
   return new NXSL_Value(new NXSL_Object(&g_nxslNetObjClass, this));
}

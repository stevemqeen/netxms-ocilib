/* 
** NetXMS - Network Management System
** Copyright (C) 2003-2014 Victor Kirhenshtein
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation; either version 3 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** File: nxsl_classes.h
**
**/

#ifndef _nxsl_classes_h_
#define _nxsl_classes_h_

#include <nms_threads.h>
#include <nms_util.h>
#include <nxcpapi.h>

//
// Constants
//

#define MAX_CLASS_NAME     64
#define INVALID_ADDRESS    ((UINT32)0xFFFFFFFF)

/**
 * NXSL data types
 */
enum NXSL_DataTypes
{
   NXSL_DT_NULL       = 0,
   NXSL_DT_OBJECT     = 1,
   NXSL_DT_ARRAY      = 2,
   NXSL_DT_ITERATOR   = 3,
   NXSL_DT_HASHMAP    = 4,
   NXSL_DT_STRING     = 5,
   NXSL_DT_REAL       = 6,
   NXSL_DT_INT32      = 7,
   NXSL_DT_INT64      = 8,
   NXSL_DT_UINT32     = 9,
   NXSL_DT_UINT64     = 10
};

/**
 * NXSL stack class
 */
class LIBNXSL_EXPORTABLE NXSL_Stack
{
private:
   int m_nStackSize;
   int m_nStackPos;
   void **m_ppData;

public:
   NXSL_Stack();
   ~NXSL_Stack();

   void push(void *pData);
   void *pop();
   void *peek();
   void *peekAt(int offset);
   void **peekList(int nLevel) { return &m_ppData[m_nStackPos - nLevel]; }

   int getSize() { return m_nStackPos; }
};

class NXSL_Value;
class NXSL_Object;
class NXSL_VM;

/**
 * External method structure
 */
struct NXSL_ExtMethod
{
   int (* handler)(NXSL_Object *object, int argc, NXSL_Value **argv, NXSL_Value **result, NXSL_VM *vm);
   int numArgs;   // Number of arguments or -1 for variable number
};

#define NXSL_METHOD_DEFINITION(clazz, name) \
   static int M_##clazz##_##name (NXSL_Object *object, int argc, NXSL_Value **argv, NXSL_Value **result, NXSL_VM *vm)

#define NXSL_REGISTER_METHOD(clazz, name, argc) { \
      NXSL_ExtMethod *m = new NXSL_ExtMethod; \
      m->handler = M_##clazz##_##name; \
      m->numArgs = argc; \
      m_methods->set(_T(#name), m); \
   }

/**
 * Class representing NXSL class
 */
class LIBNXSL_EXPORTABLE NXSL_Class
{
private:
   TCHAR m_name[MAX_CLASS_NAME];
   StringList m_classHierarchy;

protected:
   StringObjectMap<NXSL_ExtMethod> *m_methods;

   void setName(const TCHAR *name);

public:
   NXSL_Class();
   virtual ~NXSL_Class();

   virtual NXSL_Value *getAttr(NXSL_Object *object, const TCHAR *attr);
   virtual bool setAttr(NXSL_Object *object, const TCHAR *attr, NXSL_Value *value);

   virtual int callMethod(const TCHAR *name, NXSL_Object *object, int argc, NXSL_Value **argv, NXSL_Value **result, NXSL_VM *vm);

   virtual void onObjectCreate(NXSL_Object *object);
	virtual void onObjectDelete(NXSL_Object *object);

   const TCHAR *getName() const { return m_name; }
   bool instanceOf(const TCHAR *name) const { return !_tcscmp(name, m_name) || m_classHierarchy.contains(name); }
};

/**
 * Class data - object and reference count
 */
struct __nxsl_class_data
{
	void *data;
	int refCount;
};

/**
 * Object instance
 */
class LIBNXSL_EXPORTABLE NXSL_Object
{
private:
   NXSL_Class *m_class;
   __nxsl_class_data *m_data;

public:
   NXSL_Object(NXSL_Object *object);
   NXSL_Object(NXSL_Class *nxslClass, void *data);
   ~NXSL_Object();

   NXSL_Class *getClass() { return m_class; }
	void *getData() { return m_data->data; }
};

/**
 * Reference counting object
 */
class LIBNXSL_EXPORTABLE NXSL_HandleCountObject
{
protected:
   int m_handleCount;

public:
   NXSL_HandleCountObject()
   {
      m_handleCount = 0;
   }

	void incHandleCount() { m_handleCount++; }
	void decHandleCount() { m_handleCount--; }
	bool isUnused() { return m_handleCount < 1; }
   bool isShared() { return m_handleCount > 1; }
};

/**
 * Array element
 */
struct NXSL_ArrayElement
{
	int index;
	NXSL_Value *value;
};

/**
 * NXSL array
 */
class LIBNXSL_EXPORTABLE NXSL_Array : public NXSL_HandleCountObject
{
private:
	int m_size;
	int m_allocated;
	NXSL_ArrayElement *m_data;

public:
	NXSL_Array();
	NXSL_Array(const NXSL_Array *src);
   NXSL_Array(const StringList *values);
	~NXSL_Array();

   int size() const { return m_size; }
   int getMinIndex() const { return (m_size > 0) ? m_data[0].index : 0; }
   int getMaxIndex() const { return (m_size > 0) ? m_data[m_size - 1].index : 0; }

   StringList *toStringList() const;

   NXSL_Value *get(int index) const;
   NXSL_Value *getByPosition(int position) const;

   void set(int index, NXSL_Value *value);
	void add(NXSL_Value *value) { if (m_size == 0) { set(0, value); } else { set(getMaxIndex() + 1, value); } }
};

/**
 * NXSL hash map
 */
class LIBNXSL_EXPORTABLE NXSL_HashMap : public NXSL_HandleCountObject
{
private:
   StringObjectMap<NXSL_Value> *m_values;

public:
	NXSL_HashMap();
	NXSL_HashMap(const NXSL_HashMap *src);
	~NXSL_HashMap();

   void set(const TCHAR *key, NXSL_Value *value) { m_values->set(key, value); }
   NXSL_Value *get(const TCHAR *key) const { return m_values->get(key); }
   NXSL_Value *getKeys() const;
   NXSL_Value *getValues() const;

   StringMap *toStringMap() const;

	int size() const { return m_values->size(); }
};

/**
 * Iterator for arrays
 */
class LIBNXSL_EXPORTABLE NXSL_Iterator
{
private:
	int m_refCount;
	TCHAR *m_variable;
	NXSL_Array *m_array;
	int m_position;

public:
	NXSL_Iterator(const TCHAR *variable, NXSL_Array *array);
	~NXSL_Iterator();

	const TCHAR *getVariableName() { return m_variable; }

	void incRefCount() { m_refCount++; }
	void decRefCount() { m_refCount--; }
	bool isUnused() { return m_refCount < 1; }

	NXSL_Value *next();

	static int createIterator(NXSL_Stack *stack);
};

/**
 * Object handle
 */
template <class T> class NXSL_Handle
{
private:
   T *m_object;
   int m_refCount;

public:
   NXSL_Handle(T *o) 
   { 
      m_object = o; 
      o->incHandleCount(); 
      m_refCount = 0; 
   }
   NXSL_Handle(NXSL_Handle<T> *h) 
   { 
      m_object = h->m_object; 
      m_object->incHandleCount(); 
      m_refCount = 0; 
   }
   ~NXSL_Handle() 
   { 
      m_object->decHandleCount(); 
      if (m_object->isUnused()) 
         delete m_object; 
   }

	void incRefCount() { m_refCount++; }
	void decRefCount() { m_refCount--; }
	bool isUnused() { return m_refCount < 1; }
   bool isShared() { return m_refCount > 1; }

   T *getObject() { return m_object; }
   bool isSharedObject() { return m_object->isShared(); }

   void cloneObject()
   {
      m_object->decHandleCount();
      m_object = new T(m_object);
      m_object->incHandleCount();
   }
};

/**
 * Variable or constant value
 */
class LIBNXSL_EXPORTABLE NXSL_Value
{
protected:
   UINT32 m_dwStrLen;
   TCHAR *m_pszValStr;
#ifdef UNICODE
	char *m_valueMBStr;	// value as MB string; NULL until first request
#endif
	TCHAR *m_name;
   BYTE m_nDataType;
   BYTE m_bStringIsValid;
   union
   {
      INT32 nInt32;
      UINT32 uInt32;
      INT64 nInt64;
      UINT64 uInt64;
      double dReal;
      NXSL_Object *object;
		NXSL_Iterator *iterator;
		NXSL_Handle<NXSL_Array> *arrayHandle;
      NXSL_Handle<NXSL_HashMap> *hashMapHandle;
   } m_value;

   void updateNumber();
   void updateString();

   void invalidateString()
   {
      safe_free_and_null(m_pszValStr);
#ifdef UNICODE
		safe_free_and_null(m_valueMBStr);
#endif
      m_bStringIsValid = FALSE;
   }

public:
   NXSL_Value();
   NXSL_Value(const NXSL_Value *src);
   NXSL_Value(NXSL_Object *object);
   NXSL_Value(NXSL_Array *array);
   NXSL_Value(NXSL_Iterator *iterator);
   NXSL_Value(NXSL_HashMap *hashMap);
   NXSL_Value(INT32 nValue);
   NXSL_Value(INT64 nValue);
   NXSL_Value(UINT32 uValue);
   NXSL_Value(UINT64 uValue);
   NXSL_Value(double dValue);
   NXSL_Value(const TCHAR *value);
   NXSL_Value(const TCHAR *value, UINT32 len);
#ifdef UNICODE
   NXSL_Value(const char *value);
#endif
   ~NXSL_Value();

   void set(INT32 value);

	void setName(const TCHAR *name) { safe_free(m_name); m_name = _tcsdup_ex(name); }
	const TCHAR *getName() const { return m_name; }

   bool convert(int nDataType);
   int getDataType() const { return m_nDataType; }

   bool isNull() const { return (m_nDataType == NXSL_DT_NULL); }
   bool isObject() const { return (m_nDataType == NXSL_DT_OBJECT); }
	bool isObject(const TCHAR *className) const;
   bool isArray() const { return (m_nDataType == NXSL_DT_ARRAY); }
   bool isHashMap() const { return (m_nDataType == NXSL_DT_HASHMAP); }
   bool isIterator() const { return (m_nDataType == NXSL_DT_ITERATOR); }
   bool isString() const { return (m_nDataType >= NXSL_DT_STRING); }
   bool isNumeric() const { return (m_nDataType > NXSL_DT_STRING); }
   bool isReal() const { return (m_nDataType == NXSL_DT_REAL); }
   bool isInteger() const { return (m_nDataType > NXSL_DT_REAL); }
   bool isUnsigned() const { return (m_nDataType >= NXSL_DT_UINT32); }
   bool isZero() const;
   bool isNonZero() const;

   const TCHAR *getValueAsString(UINT32 *len);
   const TCHAR *getValueAsCString();
#ifdef UNICODE
   const char *getValueAsMBString();
#else
	const char *getValueAsMBString() { return getValueAsCString(); }
#endif
   INT32 getValueAsInt32();
   UINT32 getValueAsUInt32();
   INT64 getValueAsInt64();
   UINT64 getValueAsUInt64();
   double getValueAsReal();
   bool getValueAsBoolean() { return getValueAsInt32() != 0; }
   NXSL_Object *getValueAsObject() { return (m_nDataType == NXSL_DT_OBJECT) ? m_value.object : NULL; }
   NXSL_Array *getValueAsArray() { return (m_nDataType == NXSL_DT_ARRAY) ? m_value.arrayHandle->getObject() : NULL; }
   NXSL_HashMap *getValueAsHashMap() { return (m_nDataType == NXSL_DT_HASHMAP) ? m_value.hashMapHandle->getObject() : NULL; }
   NXSL_Iterator *getValueAsIterator() { return (m_nDataType == NXSL_DT_ITERATOR) ? m_value.iterator : NULL; }

   void concatenate(const TCHAR *string, UINT32 len);
   
   void increment();
   void decrement();
   void negate();
   void bitNot();

   void add(NXSL_Value *pVal);
   void sub(NXSL_Value *pVal);
   void mul(NXSL_Value *pVal);
   void div(NXSL_Value *pVal);
   void rem(NXSL_Value *pVal);
   void bitAnd(NXSL_Value *pVal);
   void bitOr(NXSL_Value *pVal);
   void bitXor(NXSL_Value *pVal);
   void lshift(int nBits);
   void rshift(int nBits);

   BOOL EQ(NXSL_Value *pVal);
   BOOL LT(NXSL_Value *pVal);
   BOOL LE(NXSL_Value *pVal);
   BOOL GT(NXSL_Value *pVal);
   BOOL GE(NXSL_Value *pVal);

   void copyOnWrite();
   void onVariableSet();

   bool equals(const NXSL_Value *v) const;
   void serialize(ByteStream& s) const;

   static NXSL_Value *load(ByteStream& s);
};

/**
 * NXSL function definition structure
 */
class NXSL_Function
{
public:
   TCHAR m_name[MAX_FUNCTION_NAME];
   UINT32 m_dwAddr;

   NXSL_Function() { m_name[0] = 0; m_dwAddr = INVALID_ADDRESS; }
   NXSL_Function(NXSL_Function *src) { nx_strncpy(m_name, src->m_name, MAX_FUNCTION_NAME); m_dwAddr = src->m_dwAddr; }
   NXSL_Function(const TCHAR *name, UINT32 addr) { nx_strncpy(m_name, name, MAX_FUNCTION_NAME); m_dwAddr = addr; }
};

/**
 * External function structure
 */
struct NXSL_ExtFunction
{
   TCHAR m_name[MAX_FUNCTION_NAME];
   int (* m_pfHandler)(int argc, NXSL_Value **argv, NXSL_Value **result, NXSL_VM *vm);
   int m_iNumArgs;   // Number of arguments or -1 for variable number
};

/**
 * External selector structure
 */
struct NXSL_ExtSelector
{
   TCHAR m_name[MAX_FUNCTION_NAME];
   int (* m_handler)(const TCHAR *name, NXSL_Value *options, int argc, NXSL_Value **argv, int *selection, NXSL_VM *vm);
};

/**
 * NXSL module import information
 */
struct NXSL_ModuleImport
{
   TCHAR name[MAX_PATH];
   int lineNumber;   // line number in source code where module was referenced
};

class NXSL_Library;

/**
 * Environment for NXSL program
 */
class LIBNXSL_EXPORTABLE NXSL_Environment
{
private:
   UINT32 m_numFunctions;
   NXSL_ExtFunction *m_functions;

   UINT32 m_numSelectors;
   NXSL_ExtSelector *m_selectors;

   NXSL_Library *m_library;

public:
   NXSL_Environment();
   virtual ~NXSL_Environment();

	virtual void print(NXSL_Value *value);
	virtual void trace(int level, const TCHAR *text);

	virtual void configureVM(NXSL_VM *vm);

   void setLibrary(NXSL_Library *lib) { m_library = lib; }

   NXSL_ExtFunction *findFunction(const TCHAR *name);
   void registerFunctionSet(UINT32 count, NXSL_ExtFunction *list);

   NXSL_ExtSelector *findSelector(const TCHAR *name);
   void registerSelectorSet(UINT32 count, NXSL_ExtSelector *list);

   bool loadModule(NXSL_VM *vm, const NXSL_ModuleImport *importInfo);
};

/**
 * Runtime variable information
 */
class LIBNXSL_EXPORTABLE NXSL_Variable
{
protected:
   TCHAR *m_pszName;
   NXSL_Value *m_pValue;
	bool m_isConstant;

public:
   NXSL_Variable(const TCHAR *pszName);
   NXSL_Variable(const TCHAR *pszName, NXSL_Value *pValue, bool constant = false);
   NXSL_Variable(NXSL_Variable *pSrc);
   ~NXSL_Variable();

   const TCHAR *getName() { return m_pszName; }
   NXSL_Value *getValue() { return m_pValue; }
   void setValue(NXSL_Value *pValue);
	bool isConstant() { return m_isConstant; }
};

/**
 * Varable system
 */
class LIBNXSL_EXPORTABLE NXSL_VariableSystem
{
protected:
   ObjectArray<NXSL_Variable> *m_variables;
	bool m_isConstant;

public:
   NXSL_VariableSystem(bool constant = false);
   NXSL_VariableSystem(NXSL_VariableSystem *src);
   ~NXSL_VariableSystem();

   NXSL_Variable *find(const TCHAR *pszName);
   NXSL_Variable *create(const TCHAR *pszName, NXSL_Value *pValue = NULL);
	void merge(NXSL_VariableSystem *src);
	void addAll(StringObjectMap<NXSL_Value> *src);
   void clear() { m_variables->clear(); }
	bool isConstant() { return m_isConstant; }
};

/**
 * Instruction's operand type
 */
enum OperandType
{
   OP_TYPE_NONE = 0,
   OP_TYPE_ADDR = 1,
   OP_TYPE_STRING = 2,
   OP_TYPE_CONST = 3
};

/**
 * Single execution instruction
 */
class LIBNXSL_EXPORTABLE NXSL_Instruction
{
   friend class NXSL_Program;
   friend class NXSL_VM;

protected:
   INT16 m_nOpCode;
   INT16 m_nStackItems;
   union
   {
      NXSL_Value *m_pConstant;
      TCHAR *m_pszString;
      UINT32 m_dwAddr;
   } m_operand;
   INT32 m_nSourceLine;

public:
   NXSL_Instruction(int nLine, short nOpCode);
   NXSL_Instruction(int nLine, short nOpCode, NXSL_Value *pValue);
   NXSL_Instruction(int nLine, short nOpCode, char *pszString);
   NXSL_Instruction(int nLine, short nOpCode, char *pszString, short nStackItems);
   NXSL_Instruction(int nLine, short nOpCode, UINT32 dwAddr);
   NXSL_Instruction(int nLine, short nOpCode, short nStackItems);
   NXSL_Instruction(NXSL_Instruction *pSrc);
   ~NXSL_Instruction();

   OperandType getOperandType();
};

/**
 * NXSL module information
 */
struct NXSL_Module
{
   TCHAR m_name[MAX_PATH];
   UINT32 m_codeStart;
   int m_codeSize;
   int m_functionStart;
   int m_numFunctions;
};

/**
 * Compiled NXSL program
 */
class LIBNXSL_EXPORTABLE NXSL_Program
{
   friend class NXSL_VM;

protected:
   ObjectArray<NXSL_Instruction> *m_instructionSet;
   ObjectArray<NXSL_ModuleImport> *m_requiredModules;
   StringObjectMap<NXSL_Value> *m_constants;
   ObjectArray<NXSL_Function> *m_functions;

	UINT32 getFinalJumpDestination(UINT32 dwAddr, int srcJump);

public:
   NXSL_Program();
   ~NXSL_Program();

   bool addFunction(const char *pszName, UINT32 dwAddr, char *pszError);
   void resolveFunctions();
   void addInstruction(NXSL_Instruction *pInstruction) { m_instructionSet->add(pInstruction); }
   void resolveLastJump(int opcode, int offset = 0);
	void createJumpAt(UINT32 dwOpAddr, UINT32 dwJumpAddr);
   void addRequiredModule(const char *name, int lineNumber);
	void optimize();
	void removeInstructions(UINT32 start, int count);
   bool addConstant(const char *name, NXSL_Value *value);

   UINT32 getCodeSize() { return m_instructionSet->size(); }

   void dump(FILE *pFile);
   void serialize(ByteStream& s);

   static NXSL_Program *load(ByteStream& s, TCHAR *errMsg, size_t errMsgSize);
};

/**
 * NXSL Script
 */
class LIBNXSL_EXPORTABLE NXSL_LibraryScript
{
protected:
   UINT32 m_id;
   uuid m_guid;
   TCHAR m_name[1024];
   TCHAR *m_source;
   NXSL_Program *m_program;
   TCHAR m_error[1024];

public:
   NXSL_LibraryScript();
   NXSL_LibraryScript(UINT32 id, uuid guid, const TCHAR *name, TCHAR *source);
   ~NXSL_LibraryScript();

   bool isValid() const { return m_program != NULL; }

   const uuid& getGuid() { return m_guid; }
   UINT32 getId() { return m_id; }

   const TCHAR *getName() { return m_name; }
   const TCHAR *getCode() { return m_source; }
   const TCHAR *getError() { return m_error; }

   NXSL_Program *getProgram() { return m_program; }

   void fillMessage(NXCPMessage *msg, UINT32 base);
   void fillMessage(NXCPMessage *msg);
};

/**
 * Script library
 */
class LIBNXSL_EXPORTABLE NXSL_Library
{
private:
   ObjectArray<NXSL_LibraryScript> *m_scriptList;
   MUTEX m_mutex;

   void deleteInternal(int nIndex);

public:
   NXSL_Library();
   ~NXSL_Library();

   void lock() { MutexLock(m_mutex); }
   void unlock() { MutexUnlock(m_mutex); }

   BOOL addScript(NXSL_LibraryScript *script);
   void deleteScript(const TCHAR *name);
   void deleteScript(UINT32 id);
   NXSL_Program *findNxslProgram(const TCHAR *name);
   NXSL_LibraryScript *findScript(UINT32 id);
   NXSL_LibraryScript *findScript(uuid guid);
   NXSL_VM *createVM(const TCHAR *name, NXSL_Environment *env);

   void fillMessage(NXCPMessage *msg);
};

/**
 * Catch point information
 */
struct NXSL_CatchPoint
{
   UINT32 addr;
   UINT32 subLevel;
   int dataStackSize;
};

/**
 * NXSL storage class - base class for actual persistent storage
 */
class LIBNXSL_EXPORTABLE NXSL_Storage
{
public:
   NXSL_Storage();
   virtual ~NXSL_Storage();

   /**
    * Write to storage. Storage becomes owner of provided value.
    * Passing NULL value will effectively remove value from storage.
    */
   virtual void write(const TCHAR *name, NXSL_Value *value) = 0;

   /**
    * Read from storage. Returns new value owned by caller. Returns NXSL NULL if there are no value with given name.
    */
   virtual NXSL_Value *read(const TCHAR *name) = 0;
};

/**
 * NXSL storage local implementation
 */
class LIBNXSL_EXPORTABLE NXSL_LocalStorage : public NXSL_Storage
{
protected:
   StringObjectMap<NXSL_Value> *m_values;

public:
   NXSL_LocalStorage();
   virtual ~NXSL_LocalStorage();

   virtual void write(const TCHAR *name, NXSL_Value *value);
   virtual NXSL_Value *read(const TCHAR *name);
};

/**
 * NXSL virtual machine
 */
class LIBNXSL_EXPORTABLE NXSL_VM
{
private:
   static EnumerationCallbackResult createConstantsCallback(const TCHAR *key, const void *value, void *data);

protected:
   NXSL_Environment *m_env;
	void *m_userData;

   ObjectArray<NXSL_Instruction> *m_instructionSet;
   UINT32 m_cp;

   UINT32 m_dwSubLevel;
   NXSL_Stack *m_dataStack;
   NXSL_Stack *m_codeStack;
   NXSL_Stack *m_catchStack;
   int m_nBindPos;

   NXSL_VariableSystem *m_constants;
   NXSL_VariableSystem *m_globals;
   NXSL_VariableSystem *m_locals;

   NXSL_Storage *m_storage;
   NXSL_Storage *m_localStorage;

   ObjectArray<NXSL_Function> *m_functions;
   ObjectArray<NXSL_Module> *m_modules;

   NXSL_Value *m_pRetValue;
   int m_errorCode;
   int m_errorLine;
   TCHAR *m_errorText;

   void execute();
   bool unwind();
   void callFunction(int nArgCount);
   UINT32 callSelector(const TCHAR *name, int numElements);
   void doUnaryOperation(int nOpCode);
   void doBinaryOperation(int nOpCode);
   void getOrUpdateArrayElement(int opcode, NXSL_Value *array, NXSL_Value *index);
   bool setArrayElement(NXSL_Value *array, NXSL_Value *index, NXSL_Value *value);
   void getArrayAttribute(NXSL_Array *a, const TCHAR *attribute, bool safe);
   void getOrUpdateHashMapElement(int opcode, NXSL_Value *hashMap, NXSL_Value *key);
   bool setHashMapElement(NXSL_Value *hashMap, NXSL_Value *key, NXSL_Value *value);
   void getHashMapAttribute(NXSL_HashMap *m, const TCHAR *attribute, bool safe);
   void error(int errorCode, int sourceLine = -1);
   NXSL_Value *matchRegexp(NXSL_Value *pValue, NXSL_Value *pRegexp, BOOL bIgnoreCase);

   NXSL_Variable *findVariable(const TCHAR *pszName);
   NXSL_Variable *findOrCreateVariable(const TCHAR *pszName);
	NXSL_Variable *createVariable(const TCHAR *pszName);

   void relocateCode(UINT32 dwStartOffset, UINT32 dwLen, UINT32 dwShift);
   UINT32 getFunctionAddress(const TCHAR *pszName);

public:
   NXSL_VM(NXSL_Environment *env = NULL, NXSL_Storage *storage = NULL);
   ~NXSL_VM();

   void loadModule(NXSL_Program *module, const NXSL_ModuleImport *importInfo);

	void setGlobalVariable(const TCHAR *pszName, NXSL_Value *pValue);
	NXSL_Variable *findGlobalVariable(const TCHAR *pszName) { return m_globals->find(pszName); }

	bool addConstant(const TCHAR *name, NXSL_Value *value);

	void setStorage(NXSL_Storage *storage);

	void storageWrite(const TCHAR *name, NXSL_Value *value) { m_storage->write(name, value); }
   void storageWrite(const TCHAR *name, const TCHAR *value) { m_storage->write(name, new NXSL_Value(value)); }
   void storageWrite(const TCHAR *name, INT32 value) { m_storage->write(name, new NXSL_Value(value)); }
   void storageWrite(const TCHAR *name, UINT32 value) { m_storage->write(name, new NXSL_Value(value)); }
   void storageWrite(const TCHAR *name, INT64 value) { m_storage->write(name, new NXSL_Value(value)); }
   void storageWrite(const TCHAR *name, UINT64 value) { m_storage->write(name, new NXSL_Value(value)); }
   void storageWrite(const TCHAR *name, double value) { m_storage->write(name, new NXSL_Value(value)); }
	NXSL_Value *storageRead(const TCHAR *name) { return m_storage->read(name); }

   bool load(NXSL_Program *program);
   bool run(ObjectArray<NXSL_Value> *args, NXSL_VariableSystem *pUserLocals = NULL,
            NXSL_VariableSystem **ppGlobals = NULL, NXSL_VariableSystem *pConstants = NULL,
            const TCHAR *entryPoint = NULL);
   bool run(int argc, NXSL_Value **argv, NXSL_VariableSystem *pUserLocals = NULL,
            NXSL_VariableSystem **ppGlobals = NULL, NXSL_VariableSystem *pConstants = NULL,
            const TCHAR *entryPoint = NULL);
   bool run() { ObjectArray<NXSL_Value> args(1, 1, false); return run(&args); }

   UINT32 getCodeSize() { return m_instructionSet->size(); }

	void trace(int level, const TCHAR *text);
   void dump(FILE *pFile);
   int getErrorCode() { return m_errorCode; }
   int getErrorLine() { return m_errorLine; }
   const TCHAR *getErrorText() { return CHECK_NULL_EX(m_errorText); }
   NXSL_Value *getResult() { return m_pRetValue; }

	void *getUserData() { return m_userData; }
	void setUserData(void *data) { m_userData = data; }
};

/**
 * NXSL "TableRow" class
 */
class LIBNXSL_EXPORTABLE NXSL_TableRowClass : public NXSL_Class
{
public:
   NXSL_TableRowClass();
   virtual ~NXSL_TableRowClass();

   virtual NXSL_Value *getAttr(NXSL_Object *pObject, const TCHAR *pszAttr);
   virtual void onObjectDelete(NXSL_Object *object);
};

/**
 * NXSL "TableColumn" class
 */
class LIBNXSL_EXPORTABLE NXSL_TableColumnClass : public NXSL_Class
{
public:
   NXSL_TableColumnClass();
   virtual ~NXSL_TableColumnClass();

   virtual NXSL_Value *getAttr(NXSL_Object *pObject, const TCHAR *pszAttr);
	virtual void onObjectDelete(NXSL_Object *object);
};

/**
 * NXSL "Table" class
 */
class LIBNXSL_EXPORTABLE NXSL_TableClass : public NXSL_Class
{
public:
   NXSL_TableClass();
   virtual ~NXSL_TableClass();

   virtual NXSL_Value *getAttr(NXSL_Object *pObject, const TCHAR *pszAttr);
	virtual void onObjectDelete(NXSL_Object *object);
};

/**
 * NXSL "StaticTable" class - table that is not deleted when ref count reaches 0
 */
class LIBNXSL_EXPORTABLE NXSL_StaticTableClass : public NXSL_TableClass
{
public:
   NXSL_StaticTableClass();
   virtual ~NXSL_StaticTableClass();

	virtual void onObjectDelete(NXSL_Object *object);
};

/**
 * NXSL "Connector" class
 */
class LIBNXSL_EXPORTABLE NXSL_ConnectorClass : public NXSL_Class
{
public:
   NXSL_ConnectorClass();
   virtual ~NXSL_ConnectorClass();

   virtual NXSL_Value *getAttr(NXSL_Object *pObject, const TCHAR *pszAttr);
	virtual void onObjectDelete(NXSL_Object *object);
};

/**
 * NXSL "Connector" class
 */
class LIBNXSL_EXPORTABLE NXSL_GeoLocationClass : public NXSL_Class
{
public:
   NXSL_GeoLocationClass();
   virtual ~NXSL_GeoLocationClass();

   virtual NXSL_Value *getAttr(NXSL_Object *pObject, const TCHAR *pszAttr);
   virtual void onObjectDelete(NXSL_Object *object);
};

/**
 * Class definition instances
 */
extern NXSL_TableClass LIBNXSL_EXPORTABLE g_nxslTableClass;
extern NXSL_StaticTableClass LIBNXSL_EXPORTABLE g_nxslStaticTableClass;
extern NXSL_TableRowClass LIBNXSL_EXPORTABLE g_nxslTableRowClass;
extern NXSL_TableColumnClass LIBNXSL_EXPORTABLE g_nxslTableColumnClass;
extern NXSL_ConnectorClass LIBNXSL_EXPORTABLE g_nxslConnectorClass;
extern NXSL_GeoLocationClass LIBNXSL_EXPORTABLE g_nxslGeoLocationClass;

#endif

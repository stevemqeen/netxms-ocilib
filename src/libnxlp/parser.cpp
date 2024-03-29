/*
** NetXMS - Network Management System
** Log Parsing Library
** Copyright (C) 2003-2017 Raden Solutions
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
** File: parser.cpp
**
**/

#include "libnxlp.h"
#include <expat.h>

/**
 * Context state texts
 */
static const TCHAR *s_states[] = { _T("MANUAL"), _T("AUTO"), _T("INACTIVE") };

/**
 * XML parser state codes
 */
#define XML_STATE_INIT        -1
#define XML_STATE_END         -2
#define XML_STATE_ERROR       -255
#define XML_STATE_PARSER      0
#define XML_STATE_RULES       1
#define XML_STATE_RULE        2
#define XML_STATE_MATCH       3
#define XML_STATE_EVENT       4
#define XML_STATE_FILE        5
#define XML_STATE_ID          6
#define XML_STATE_LEVEL       7
#define XML_STATE_SOURCE      8
#define XML_STATE_CONTEXT     9
#define XML_STATE_MACROS      10
#define XML_STATE_MACRO       11
#define XML_STATE_DESCRIPTION 12

/**
 * XML parser state for creating LogParser object from XML
 */
struct XML_PARSER_STATE
{
	LogParser *parser;
	int state;
	String regexp;
	String event;
	String file;
	StringList files;
	IntegerArray<INT32> encodings;
   IntegerArray<INT32> preallocFlags;
	String id;
	String level;
	String source;
	String context;
	String description;
	String ruleName;
	int contextAction;
	String ruleContext;
	int numEventParams;
	String errorText;
	String macroName;
	String macro;
	bool invertedRule;
	bool breakFlag;
	int repeatCount;
	int repeatInterval;
	bool resetRepeat;

	XML_PARSER_STATE() : encodings(4, 4), preallocFlags(4, 4)
	{
      state = -1;
      parser = NULL;
	   invertedRule = false;
	   breakFlag = false;
	   contextAction = CONTEXT_SET_AUTOMATIC;
	   numEventParams = 0;
	   repeatCount = 0;
	   repeatInterval = 0;
	   resetRepeat = true;
	}
};

/**
 * Parser default constructor
 */
LogParser::LogParser()
{
   m_rules = new ObjectArray<LogParserRule>(16, 16, true);
	m_cb = NULL;
	m_userArg = NULL;
	m_name = NULL;
	m_fileName = NULL;
	m_fileEncoding = LP_FCP_ACP;
	m_preallocatedFile = false;
	m_eventNameList = NULL;
	m_eventResolver = NULL;
	m_thread = INVALID_THREAD_HANDLE;
	m_recordsProcessed = 0;
	m_recordsMatched = 0;
	m_processAllRules = false;
	m_traceLevel = 0;
	m_traceCallback = NULL;
	_tcscpy(m_status, LPS_INIT);
#ifdef _WIN32
   m_marker = NULL;
#endif
}

/**
 * Parser copy constructor
 */
LogParser::LogParser(LogParser *src)
{
   int count = src->m_rules->size();
   m_rules = new ObjectArray<LogParserRule>(count, 16, true);
	for(int i = 0; i < count; i++)
		m_rules->add(new LogParserRule(src->m_rules->get(i), this));

	m_cb = src->m_cb;
	m_userArg = src->m_userArg;
	m_name = _tcsdup_ex(src->m_name);
	m_fileName = _tcsdup_ex(src->m_fileName);
	m_fileEncoding = src->m_fileEncoding;
	m_preallocatedFile = src->m_preallocatedFile;

	if (src->m_eventNameList != NULL)
	{
		int count;
		for(count = 0; src->m_eventNameList[count].text != NULL; count++);
		m_eventNameList = (count > 0) ? (CODE_TO_TEXT *)nx_memdup(src->m_eventNameList, sizeof(CODE_TO_TEXT) * (count + 1)) : NULL;
	}
	else
	{
		m_eventNameList = NULL;
	}

	m_eventResolver = src->m_eventResolver;
	m_thread = INVALID_THREAD_HANDLE;
	m_recordsProcessed = 0;
	m_recordsMatched = 0;
	m_processAllRules = src->m_processAllRules;
	m_traceLevel = src->m_traceLevel;
	m_traceCallback = src->m_traceCallback;
	_tcscpy(m_status, LPS_INIT);
#ifdef _WIN32
   m_marker = _tcsdup_ex(src->m_marker);
#endif
}

/**
 * Destructor
 */
LogParser::~LogParser()
{
   delete m_rules;
	free(m_name);
	free(m_fileName);
#ifdef _WIN32
   free(m_marker);
#endif
}

/**
 * Trace
 */
void LogParser::trace(int level, const TCHAR *format, ...)
{
	va_list args;

	if ((m_traceCallback == NULL) || (level > m_traceLevel))
		return;

	va_start(args, format);
	m_traceCallback(level, format, args);
	va_end(args);
}

/**
 * Add rule
 */
bool LogParser::addRule(LogParserRule *rule)
{
	bool valid = rule->isValid();
	if (valid)
	{
	   m_rules->add(rule);
	}
	else
	{
		delete rule;
	}
	return valid;
}

/**
 * Create and add rule
 */
bool LogParser::addRule(const TCHAR *regexp, UINT32 eventCode, const TCHAR *eventName, int numParams, int repeatInterval, int repeatCount, bool resetRepeat)
{
	return addRule(new LogParserRule(this, NULL, regexp, eventCode, eventName, numParams, repeatInterval, repeatCount, resetRepeat));
}

/**
 * Check context
 */
const TCHAR *LogParser::checkContext(LogParserRule *rule)
{
	const TCHAR *state;

	if (rule->getContext() == NULL)
	{
		trace(5, _T("  rule has no context"));
		return s_states[CONTEXT_SET_MANUAL];
	}

	state = m_contexts.get(rule->getContext());
	if (state == NULL)
	{
		trace(5, _T("  context '%s' inactive, rule should be skipped"), rule->getContext());
		return NULL;	// Context inactive, don't use this rule
	}

	if (!_tcscmp(state, s_states[CONTEXT_CLEAR]))
	{
		trace(5, _T("  context '%s' inactive, rule should be skipped"), rule->getContext());
		return NULL;
	}
	else
	{
		trace(5, _T("  context '%s' active (mode=%s)"), rule->getContext(), state);
		return state;
	}
}

/**
 * Match log record
 */
bool LogParser::matchLogRecord(bool hasAttributes, const TCHAR *source, UINT32 eventId,
										 UINT32 level, const TCHAR *line, UINT32 objectId)
{
	const TCHAR *state;
	bool matched = false;

	if (hasAttributes)
		trace(5, _T("Match event: source=\"%s\" id=%u level=%d text=\"%s\""), source, eventId, level, line);
	else
		trace(5, _T("Match line: \"%s\""), line);

	m_recordsProcessed++;
	int i;
	for(i = 0; i < m_rules->size(); i++)
	{
	   LogParserRule *rule = m_rules->get(i);
		trace(6, _T("checking rule %d \"%s\""), i + 1, rule->getDescription());
		if ((state = checkContext(rule)) != NULL)
		{
			bool ruleMatched = hasAttributes ?
			   rule->matchEx(source, eventId, level, line, m_cb, objectId, m_userArg) :
				rule->match(line, m_cb, objectId, m_userArg);
			if (ruleMatched)
			{
				trace(5, _T("rule %d \"%s\" matched"), i + 1, rule->getDescription());
				if (!matched)
					m_recordsMatched++;

				// Update context
				if (rule->getContextToChange() != NULL)
				{
					m_contexts.set(rule->getContextToChange(), s_states[rule->getContextAction()]);
					trace(5, _T("rule %d \"%s\": context %s set to %s"), i + 1,
					      rule->getDescription(), rule->getContextToChange(), s_states[rule->getContextAction()]);
				}

				// Set context of this rule to inactive if rule context mode is "automatic reset"
				if (!_tcscmp(state, s_states[CONTEXT_SET_AUTOMATIC]))
				{
					m_contexts.set(rule->getContext(), s_states[CONTEXT_CLEAR]);
					trace(5, _T("rule %d \"%s\": context %s cleared because it was set to automatic reset mode"),
							i + 1, rule->getDescription(), rule->getContext());
				}
				matched = true;
				if (!m_processAllRules || rule->getBreakFlag())
					break;
			}
		}
	}
	if (i < m_rules->size())
		trace(5, _T("processing stopped at rule %d \"%s\"; result = %s"), i + 1,
				m_rules->get(i)->getDescription(), matched ? _T("true") : _T("false"));
	else
		trace(5, _T("Processing stopped at end of rules list; result = %s"), matched ? _T("true") : _T("false"));
	return matched;
}

/**
 * Match simple log line
 */
bool LogParser::matchLine(const TCHAR *line, UINT32 objectId)
{
	return matchLogRecord(false, NULL, 0, 0, line, objectId);
}

/**
 * Match log event (text with additional attributes - source, severity, event id)
 */
bool LogParser::matchEvent(const TCHAR *source, UINT32 eventId, UINT32 level, const TCHAR *line, UINT32 objectId)
{
	return matchLogRecord(true, source, eventId, level, line, objectId);
}

/**
 * Set associated file name
 */
void LogParser::setFileName(const TCHAR *name)
{
	free(m_fileName);
	m_fileName = (name != NULL) ? _tcsdup(name) : NULL;
	if (m_name == NULL)
		m_name = _tcsdup(name);	// Set parser name to file name
}

/**
 * Set parser name
 */
void LogParser::setName(const TCHAR *name)
{
	safe_free(m_name);
	m_name = _tcsdup((name != NULL) ? name : CHECK_NULL(m_fileName));
}

/**
 * Add macro
 */
void LogParser::addMacro(const TCHAR *name, const TCHAR *value)
{
	m_macros.set(name, value);
}

/**
 * Get macro
 */
const TCHAR *LogParser::getMacro(const TCHAR *name)
{
	const TCHAR *value;

	value = m_macros.get(name);
	return CHECK_NULL_EX(value);
}

/**
 * Create parser configuration from XML - callback for element start
 */
static void StartElement(void *userData, const char *name, const char **attrs)
{
	XML_PARSER_STATE *ps = (XML_PARSER_STATE *)userData;

	if (!strcmp(name, "parser"))
	{
		ps->state = XML_STATE_PARSER;
		ps->parser->setProcessAllFlag(XMLGetAttrBoolean(attrs, "processAll", false));
		ps->parser->setTraceLevel(XMLGetAttrInt(attrs, "trace", 0));
		const char *name = XMLGetAttr(attrs, "name");
		if (name != NULL)
		{
#ifdef UNICODE
			WCHAR *wname = WideStringFromUTF8String(name);
			ps->parser->setName(wname);
			free(wname);
#else
			ps->parser->setName(name);
#endif
		}
	}
	else if (!strcmp(name, "file"))
	{
		ps->state = XML_STATE_FILE;
		const char *encoding = XMLGetAttr(attrs, "encoding");
		if (encoding != NULL)
		{
			if ((*encoding == 0))
			{
				ps->encodings.add(LP_FCP_AUTO);
			}
			if (!stricmp(encoding, "acp"))
			{
				ps->encodings.add(LP_FCP_ACP);
			}
			else if (!stricmp(encoding, "utf8") || !stricmp(encoding, "utf-8"))
			{
				ps->encodings.add(LP_FCP_UTF8);
			}
			else if (!stricmp(encoding, "ucs2") || !stricmp(encoding, "ucs-2") || !stricmp(encoding, "utf-16"))
			{
				ps->encodings.add(LP_FCP_UCS2);
			}
			else if (!stricmp(encoding, "ucs2le") || !stricmp(encoding, "ucs-2le") || !stricmp(encoding, "utf-16le"))
			{
				ps->encodings.add(LP_FCP_UCS2_LE);
			}
			else if (!stricmp(encoding, "ucs2be") || !stricmp(encoding, "ucs-2be") || !stricmp(encoding, "utf-16be"))
			{
				ps->encodings.add(LP_FCP_UCS2_BE);
			}
			else if (!stricmp(encoding, "ucs4") || !stricmp(encoding, "ucs-4") || !stricmp(encoding, "utf-32"))
			{
				ps->encodings.add(LP_FCP_UCS4);
			}
			else if (!stricmp(encoding, "ucs4le") || !stricmp(encoding, "ucs-4le") || !stricmp(encoding, "utf-32le"))
			{
				ps->encodings.add(LP_FCP_UCS4_LE);
			}
			else if (!stricmp(encoding, "ucs4be") || !stricmp(encoding, "ucs-4be") || !stricmp(encoding, "utf-32be"))
			{
				ps->encodings.add(LP_FCP_UCS4_BE);
			}
			else
			{
				ps->errorText = _T("Invalid file encoding");
				ps->state = XML_STATE_ERROR;
			}
		}
		else
		{
			ps->encodings.add(LP_FCP_AUTO);
		}
		ps->preallocFlags.add(XMLGetAttrBoolean(attrs, "preallocated", false) ? 1 : 0);
	}
	else if (!strcmp(name, "macros"))
	{
		ps->state = XML_STATE_MACROS;
	}
	else if (!strcmp(name, "macro"))
	{
		const char *name;

		ps->state = XML_STATE_MACRO;
		name = XMLGetAttr(attrs, "name");
#ifdef UNICODE
		ps->macroName = L"";
		ps->macroName.appendMBString(name, strlen(name), CP_UTF8);
#else
		ps->macroName = CHECK_NULL_A(name);
#endif
		ps->macro = NULL;
	}
	else if (!strcmp(name, "rules"))
	{
		ps->state = XML_STATE_RULES;
	}
	else if (!strcmp(name, "rule"))
	{
		ps->regexp = NULL;
		ps->invertedRule = false;
		ps->event = NULL;
		ps->context = NULL;
		ps->contextAction = CONTEXT_SET_AUTOMATIC;
		ps->description = NULL;
		ps->id = NULL;
		ps->source = NULL;
		ps->level = NULL;
#ifdef UNICODE
		ps->ruleContext.clear();
		const char *context = XMLGetAttr(attrs, "context");
		if (context != NULL)
			ps->ruleContext.appendMBString(context, strlen(context), CP_UTF8);
      ps->ruleName.clear();
      const char *name = XMLGetAttr(attrs, "name");
      if (name != NULL)
         ps->ruleName.appendMBString(name, strlen(name), CP_UTF8);
#else
		ps->ruleContext = XMLGetAttr(attrs, "context");
      ps->ruleName = XMLGetAttr(attrs, "name");
#endif
		ps->breakFlag = XMLGetAttrBoolean(attrs, "break", false);
		ps->state = XML_STATE_RULE;
		ps->numEventParams = 0;
	}
	else if (!strcmp(name, "match"))
	{
		ps->state = XML_STATE_MATCH;
		ps->invertedRule = XMLGetAttrBoolean(attrs, "invert", false);
		ps->resetRepeat = XMLGetAttrBoolean(attrs, "reset", true);
		ps->repeatCount = XMLGetAttrInt(attrs, "repeatCount", 0);
		ps->repeatInterval = XMLGetAttrInt(attrs, "repeatInterval", 0);
	}
	else if (!strcmp(name, "id") || !strcmp(name, "facility"))
	{
		ps->state = XML_STATE_ID;
	}
	else if (!strcmp(name, "level") || !strcmp(name, "severity"))
	{
		ps->state = XML_STATE_LEVEL;
	}
	else if (!strcmp(name, "source") || !strcmp(name, "tag"))
	{
		ps->state = XML_STATE_SOURCE;
	}
	else if (!strcmp(name, "event"))
	{
		ps->numEventParams = XMLGetAttrUINT32(attrs, "params", 0);
		ps->state = XML_STATE_EVENT;
	}
	else if (!strcmp(name, "context"))
	{
		const char *action;

		ps->state = XML_STATE_CONTEXT;

		action = XMLGetAttr(attrs, "action");
		if (action == NULL)
			action = "set";

		if (!strcmp(action, "set"))
		{
			const char *mode;

			mode = XMLGetAttr(attrs, "reset");
			if (mode == NULL)
				mode = "auto";

			if (!strcmp(mode, "auto"))
			{
				ps->contextAction = CONTEXT_SET_AUTOMATIC;
			}
			else if (!strcmp(mode, "manual"))
			{
				ps->contextAction = CONTEXT_SET_MANUAL;
			}
			else
			{
				ps->errorText = _T("Invalid context reset mode");
				ps->state = XML_STATE_ERROR;
			}
		}
		else if (!strcmp(action, "clear"))
		{
			ps->contextAction = CONTEXT_CLEAR;
		}
		else
		{
			ps->errorText = _T("Invalid context action");
			ps->state = XML_STATE_ERROR;
		}
	}
	else if (!strcmp(name, "description"))
	{
		ps->state = XML_STATE_DESCRIPTION;
	}
	else
	{
		ps->state = XML_STATE_ERROR;
	}
}

/**
 * Create parser configuration from XML - callback for element end
 */
static void EndElement(void *userData, const char *name)
{
	XML_PARSER_STATE *ps = (XML_PARSER_STATE *)userData;

	if (ps->state == XML_STATE_ERROR)
      return;

	if (!strcmp(name, "parser"))
	{
		ps->state = XML_STATE_END;
	}
	else if (!strcmp(name, "file"))
	{
		ps->files.add(ps->file);
		ps->file = _T("");
		ps->state = XML_STATE_PARSER;
	}
	else if (!strcmp(name, "macros"))
	{
		ps->state = XML_STATE_PARSER;
	}
	else if (!strcmp(name, "macro"))
	{
		ps->parser->addMacro(ps->macroName, ps->macro);
		ps->state = XML_STATE_MACROS;
	}
	else if (!strcmp(name, "rules"))
	{
		ps->state = XML_STATE_PARSER;
	}
	else if (!strcmp(name, "rule"))
	{
		UINT32 eventCode;
		const TCHAR *eventName = NULL;
		TCHAR *eptr;
		LogParserRule *rule;

		ps->event.trim();
		eventCode = _tcstoul(ps->event, &eptr, 0);
		if (*eptr != 0)
		{
			eventCode = ps->parser->resolveEventName(ps->event, 0);
			if (eventCode == 0)
			{
				eventName = (const TCHAR *)ps->event;
			}
		}

		if (ps->regexp.isEmpty())
			ps->regexp = _T(".*");
		rule = new LogParserRule(ps->parser, (const TCHAR *)ps->ruleName, (const TCHAR *)ps->regexp, eventCode, eventName, ps->numEventParams, ps->repeatInterval, ps->repeatCount, ps->resetRepeat);
		if (!ps->ruleContext.isEmpty())
			rule->setContext(ps->ruleContext);
		if (!ps->context.isEmpty())
		{
			rule->setContextToChange(ps->context);
			rule->setContextAction(ps->contextAction);
		}

		if (!ps->description.isEmpty())
			rule->setDescription(ps->description);

		if (!ps->source.isEmpty())
			rule->setSource(ps->source);

		if (!ps->level.isEmpty())
			rule->setLevel(_tcstoul(ps->level, NULL, 0));

		if (!ps->id.isEmpty())
		{
			UINT32 start, end;
			TCHAR *eptr;

			start = _tcstoul(ps->id, &eptr, 0);
			if (*eptr == 0)
			{
				end = start;
			}
			else	/* TODO: add better error handling */
			{
				while(!_istdigit(*eptr))
					eptr++;
				end = _tcstoul(eptr, NULL, 0);
			}
			rule->setIdRange(start, end);
		}

		rule->setInverted(ps->invertedRule);
		rule->setBreakFlag(ps->breakFlag);

		ps->parser->addRule(rule);
		ps->state = XML_STATE_RULES;
	}
	else if (!strcmp(name, "match"))
	{
		ps->state = XML_STATE_RULE;
	}
	else if (!strcmp(name, "id") || !strcmp(name, "facility"))
	{
		ps->state = XML_STATE_RULE;
	}
	else if (!strcmp(name, "level") || !strcmp(name, "severity"))
	{
		ps->state = XML_STATE_RULE;
	}
	else if (!strcmp(name, "source") || !strcmp(name, "tag"))
	{
		ps->state = XML_STATE_RULE;
	}
	else if (!strcmp(name, "event"))
	{
		ps->state = XML_STATE_RULE;
	}
	else if (!strcmp(name, "context"))
	{
		ps->state = XML_STATE_RULE;
	}
	else if (!strcmp(name, "description"))
	{
		ps->state = XML_STATE_RULE;
	}
}

/**
 * Create parser configuration from XML - callback for element data
 */
static void CharData(void *userData, const XML_Char *s, int len)
{
	XML_PARSER_STATE *ps = (XML_PARSER_STATE *)userData;

	switch(ps->state)
	{
		case XML_STATE_MATCH:
			ps->regexp.appendMBString(s, len, CP_UTF8);
			break;
		case XML_STATE_ID:
			ps->id.appendMBString(s, len, CP_UTF8);
			break;
		case XML_STATE_LEVEL:
			ps->level.appendMBString(s, len, CP_UTF8);
			break;
		case XML_STATE_SOURCE:
			ps->source.appendMBString(s, len, CP_UTF8);
			break;
		case XML_STATE_EVENT:
			ps->event.appendMBString(s, len, CP_UTF8);
			break;
		case XML_STATE_FILE:
			ps->file.appendMBString(s, len, CP_UTF8);
			break;
		case XML_STATE_CONTEXT:
			ps->context.appendMBString(s, len, CP_UTF8);
			break;
		case XML_STATE_DESCRIPTION:
			ps->description.appendMBString(s, len, CP_UTF8);
			break;
		case XML_STATE_MACRO:
			ps->macro.appendMBString(s, len, CP_UTF8);
			break;
		default:
			break;
	}
}

/**
 * Create parser configuration from XML. Returns array of identical parsers,
 * one for each <file> tag in source XML. Resulting parsers only differs with file names.
 */
ObjectArray<LogParser> *LogParser::createFromXml(const char *xml, int xmlLen, TCHAR *errorText, int errBufSize, bool (*eventResolver)(const TCHAR *, UINT32 *))
{
	ObjectArray<LogParser> *parsers = NULL;

	XML_Parser parser = XML_ParserCreate(NULL);
	XML_PARSER_STATE state;
	state.parser = new LogParser;
	state.parser->setEventNameResolver(eventResolver);
	XML_SetUserData(parser, &state);
	XML_SetElementHandler(parser, StartElement, EndElement);
	XML_SetCharacterDataHandler(parser, CharData);
	bool success = (XML_Parse(parser, xml, (xmlLen == -1) ? (int)strlen(xml) : xmlLen, TRUE) != XML_STATUS_ERROR);
	if (!success && (errorText != NULL))
	{
		_sntprintf(errorText, errBufSize, _T("%hs at line %d"),
			XML_ErrorString(XML_GetErrorCode(parser)),
			(int)XML_GetCurrentLineNumber(parser));
	}
	XML_ParserFree(parser);
	if (success && (state.state == XML_STATE_ERROR))
	{
		if (errorText != NULL)
			nx_strncpy(errorText, state.errorText, errBufSize);
	}
	else if (success)
	{
		parsers = new ObjectArray<LogParser>;
		if (state.files.size() > 0)
		{
			for(int i = 0; i < state.files.size(); i++)
			{
				LogParser *p = (i > 0) ? new LogParser(state.parser) : state.parser;
				p->setFileName(state.files.get(i));
				p->m_fileEncoding = state.encodings.get(i);
				p->m_preallocatedFile = (state.preallocFlags.get(i) != 0);
				parsers->add(p);
			}
		}
		else
		{
			// It is possible to have parser without <file> tag, syslog parser for example
			parsers->add(state.parser);
		}
	}

	return parsers;
}

/**
 * Resolve event name
 */
UINT32 LogParser::resolveEventName(const TCHAR *name, UINT32 defVal)
{
	if (m_eventNameList != NULL)
	{
		int i;

		for(i = 0; m_eventNameList[i].text != NULL; i++)
			if (!_tcsicmp(name, m_eventNameList[i].text))
				return m_eventNameList[i].code;
	}

	if (m_eventResolver != NULL)
	{
		UINT32 val;

		if (m_eventResolver(name, &val))
			return val;
	}

	return defVal;
}

/**
 * Find rule by name
 */
const LogParserRule *LogParser::findRuleByName(const TCHAR *name) const
{
   for(int i = 0; i < m_rules->size(); i++)
   {
      LogParserRule *rule = m_rules->get(i);
      if (!_tcsicmp(rule->getName(), name))
         return rule;
   }
   return NULL;
}

/**
 * Restore counters from previous parser copy
 */
void LogParser::restoreCounters(const LogParser *parser)
{
   for(int i = 0; i < m_rules->size(); i++)
   {
      const LogParserRule *rule = parser->findRuleByName(m_rules->get(i)->getName());
      if (rule != NULL)
      {
         m_rules->get(i)->restoreCounters(rule);
      }
   }
}

/**
 * Get character size in bytes for parser's encoding
 */
int LogParser::getCharSize() const
{
   switch(m_fileEncoding)
   {
      case LP_FCP_UCS4_BE:
      case LP_FCP_UCS4_LE:
      case LP_FCP_UCS4:
         return 4;
      case LP_FCP_UCS2_BE:
      case LP_FCP_UCS2_LE:
      case LP_FCP_UCS2:
         return 2;
      default:
         return 1;
   }
}

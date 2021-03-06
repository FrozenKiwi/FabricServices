// Copyright (c) 2010-2017 Fabric Software Inc. All rights reserved.

#include "KLFunction.h"
#include "KLMethod.h"
#include "KLExtension.h"
#include "KLASTManager.h"

#include <vector>
#include <string>
#include <sstream>

using namespace FabricServices::ASTWrapper;

KLFunction::KLFunction(const KLFile* klFile, const KLNameSpace * nameSpace, JSONData data)
: KLStmt(klFile, nameSpace, data)
{
  const char * name = getStringDictValue("name");
  if(name)
    m_name = name;

  JSONData flagsVal = getDictValue("flags");
  if(flagsVal)
    m_flags = flagsVal->getSInt32();
  else
    m_flags = 0;

  const char * access = getStringDictValue("access");
  if(access)
    m_access = access;

  const char * returnType = getStringDictValue("returnType");
  if(returnType)
    m_returnType = returnType;

  const char * symbolName = getStringDictValue("symbolName");
  if(symbolName)
    m_symbolName = symbolName;

  JSONData params = getArrayDictValue("params");
  if(params)
  {
    for(uint32_t i=0;i<params->getArraySize();i++)
    {
      KLParameter * param = new KLParameter(klFile, nameSpace, params->getArrayElement(i));
      m_params.push_back(param);
    }
  }

  JSONData body = getDictValue("body");
  if(body)
    m_body = (KLCompoundStmt *)constructChild(body);
  else
    m_body = NULL;

}

KLFunction::~KLFunction()
{
  for(uint32_t i=0;i<m_params.size();i++)
  {
    delete(m_params[i]);
  }
}

KLDeclType KLFunction::getDeclType() const
{
  return KLDeclType_Function;
}

bool KLFunction::isOfDeclType(KLDeclType type) const
{
  if(type == KLDeclType_Function)
    return true;
  return KLStmt::isOfDeclType(type);
}

const std::string & KLFunction::getName() const
{
  return m_name;
}

std::string KLFunction::getReturnType(bool includeNameSpace) const
{
  if(includeNameSpace)
  {
    const KLType * klType = getASTManager()->getKLTypeByName(m_returnType.c_str(), this);
    if(klType)
    {
      return klType->getNameSpacePrefix() + m_returnType;
    }
  }
  return m_returnType;
}

const std::string & KLFunction::getSymbolName() const
{
  return m_symbolName;
}

uint32_t KLFunction::getParameterCount() const
{
  return m_params.size();
}

const KLParameter * KLFunction::getParameter(uint32_t index) const
{
  return m_params[index];
}

const KLCompoundStmt * KLFunction::getBody() const
{
  return m_body;
}

bool KLFunction::hasUniqueName() const
{
  std::vector<const KLFunction*> funcs = getExtension()->getFunctions();

  for(size_t i=0;i<funcs.size();i++)
  {
    if(funcs[i] == this)
      continue;
    if(funcs[i]->getName() == getName())
      return false;
  }

  return true;
}

bool KLFunction::isMethod() const
{
  return false;
}

bool KLFunction::isPrivate() const
{
  return m_access == "private";
}

std::string KLFunction::getPrefix() const
{
  return "";
}

std::string KLFunction::getSuffix() const
{
  return "";
}

const char * KLFunction::getKLType() const
{
  return "function";
}

std::string KLFunction::getKLCode(bool includeReturnType, bool includeKeyWord, bool includePrefix, bool includeName) const
{
  std::string code;

  if(includeKeyWord)
  {
    code += getKLType();
    code += " ";
  }

  if(m_returnType.length() > 0 && includeReturnType)
  {
    code += m_returnType;
    code += " ";
  }

  if(includePrefix)
    code += getPrefix();

  if(includeName)
    code += getName();

  bool skipSuffix = false;

  if(isMethod())
  {
    const KLMethod * method = (const KLMethod *)this;
    if("~"+method->getThisType() == getName())
      skipSuffix = true;
  }

  if(!skipSuffix)
    code += getSuffix();

  code += "(";

  if(m_params.size() > 0)
  {
    code += " ";

    for(uint32_t i=0;i<m_params.size();i++)
    {
      const KLParameter * p = m_params[i];
      if(i > 0)
        code += ", ";

      code += p->getUsage();
      code += " ";
      code += p->getTypeNoArray();
      code += " ";
      code += p->getName();
      code += p->getTypeArraySuffix();
    }


    code += " ";
  }

  code += ")";

  return code;
}

std::string KLFunction::getLabel() const
{
  if(m_label.length() == 0)
    m_label = getKLCode(false, false, false, true);
  return m_label;
}


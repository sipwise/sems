/*
 * Copyright (C) 2002-2003 Fhg Fokus
 *
 * This file is part of SEMS, a free SIP media server.
 *
 * SEMS is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. This program is released under
 * the GPL with the additional exemption that compiling, linking,
 * and/or using OpenSSL is allowed.
 *
 * For a license to use the SEMS software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * SEMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "AmArg.h"
#include "log.h"
#include "AmUtils.h"

const char* AmArg::t2str(int type) {
  switch (type) {
  case AmArg::Undef:   return "Undef";
  case AmArg::Int:     return "Int";
  case AmArg::LongLong: return "LongLong";
  case AmArg::Bool:    return "Bool";
  case AmArg::Double:  return "Double";
  case AmArg::CStr:    return "CStr";
  case AmArg::AObject: return "AObject";
  case AmArg::ADynInv: return "ADynInv";
  case AmArg::Blob:    return "Blob";
  case AmArg::Array:   return "Array";
  case AmArg::Struct:  return "Struct";
  default: return "unknown";
  }
}

AmArg::AmArg(std::map<std::string, std::string>& v)
  : type(Undef) {
  assertStruct();
  for (std::map<std::string, std::string>::iterator it=
	 v.begin();it!= v.end();it++)
    std::get<ValueStruct>(value)[it->first] = AmArg(it->second.c_str());
}

AmArg::AmArg(std::map<std::string, AmArg>& v)
  : type(Undef) {
  assertStruct();
  for (std::map<std::string, AmArg>::iterator it=
	 v.begin();it!= v.end();it++)
    std::get<ValueStruct>(value)[it->first] = it->second;
}

AmArg::AmArg(vector<std::string>& v)
  : type(Array) {
  assertArray(0);
  for (vector<std::string>::iterator it
	 = v.begin(); it != v.end(); it++) {
    push(AmArg(it->c_str()));
  }
}

AmArg::AmArg(const vector<int>& v )
  : type(Array) {
  assertArray(0);
  for (vector<int>::const_iterator it
	 = v.begin(); it != v.end(); it++) {
    push(AmArg(*it));
  }
}

AmArg::AmArg(const vector<double>& v)
  : type(Array) {
  assertArray(0);
  for (vector<double>::const_iterator it
	 = v.begin(); it != v.end(); it++) {
    push(AmArg(*it));
  }
}

void AmArg::assertArray() {
  if (Array == type)
    return;
  if (Undef == type) {
    type = Array;
    value = ValueArray();
    return;
  }
  throw TypeMismatchException();
}

void AmArg::assertArray() const {
  if (Array != type)
    throw TypeMismatchException();
}

void AmArg::assertArray(size_t s) {

  if (Undef == type) {
    type = Array;
    value = ValueArray();
  } else if (Array != type) {
    throw TypeMismatchException();
  }
  if (std::get<ValueArray>(value).size() < s)
    std::get<ValueArray>(value).resize(s);
}

void AmArg::assertStruct() {
  if (Struct == type)
    return;
  if (Undef == type) {
    type = Struct;
    value = ValueStruct();
    return;
  }
  throw TypeMismatchException();
}

void AmArg::assertStruct() const {
  if (Struct != type)
    throw TypeMismatchException();
}

void AmArg::invalidate() {
  type = Undef;
  value = std::monostate();
}

void AmArg::push(const AmArg& a) {
  assertArray();
  std::get<ValueArray>(value).push_back(a);
}

void AmArg::push(const string &key, const AmArg &val) {
  assertStruct();
  std::get<ValueStruct>(value)[key] = val;
}

void AmArg::pop(AmArg &a) {
  assertArray();
  if (!size()) {
    if (a.getType() == AmArg::Undef)
      return;
    a = AmArg();
    return;
  }
  a = std::get<ValueArray>(value).front();
  std::get<ValueArray>(value).erase(std::get<ValueArray>(value).begin());
}

void AmArg::pop_back(AmArg &a) {
  assertArray();
  if (!size()) {
    if (a.getType() == AmArg::Undef)
      return;
    a = AmArg();
    return;
  }
  a = std::get<ValueArray>(value).back();
  std::get<ValueArray>(value).erase(std::get<ValueArray>(value).end());
}

void AmArg::pop_back() {
  assertArray();
  if (!size())
    return;
  std::get<ValueArray>(value).erase(std::get<ValueArray>(value).end());
}

void AmArg::concat(const AmArg& a) {
  assertArray();
  if (a.getType() == Array) {
  for (size_t i=0;i<a.size();i++)
    std::get<ValueArray>(value).push_back(a[i]);
  } else {
    std::get<ValueArray>(value).push_back(a);
  }
}

size_t AmArg::size() const {
  if (Array == type)
    return std::get<ValueArray>(value).size();

  if (Struct == type)
    return std::get<ValueStruct>(value).size();

  throw TypeMismatchException();
}

AmArg& AmArg::back() {
  assertArray();
  if (!std::get<ValueArray>(value).size())
    throw OutOfBoundsException();

  return std::get<ValueArray>(value)[std::get<ValueArray>(value).size()-1];
}

const AmArg& AmArg::back() const {
  assertArray();
  if (!std::get<ValueArray>(value).size())
    throw OutOfBoundsException();

  return std::get<ValueArray>(value)[std::get<ValueArray>(value).size()-1];
}

AmArg& AmArg::get(size_t idx) {
  assertArray();
  if (idx >= std::get<ValueArray>(value).size())
    throw OutOfBoundsException();

  return std::get<ValueArray>(value)[idx];
}

const AmArg& AmArg::get(size_t idx) const {
  assertArray();
  if (idx >= std::get<ValueArray>(value).size())
    throw OutOfBoundsException();

  return std::get<ValueArray>(value)[idx];
}

AmArg& AmArg::operator[](size_t idx) {
  assertArray(idx+1);
  return std::get<ValueArray>(value)[idx];
}

const AmArg& AmArg::operator[](size_t idx) const {
  assertArray();
  if (idx >= std::get<ValueArray>(value).size())
    throw OutOfBoundsException();

  return std::get<ValueArray>(value)[idx];
}

AmArg& AmArg::operator[](int idx) {
  if (idx<0)
    throw OutOfBoundsException();

  assertArray(idx+1);
  return std::get<ValueArray>(value)[idx];
}

const AmArg& AmArg::operator[](int idx) const {
  if (idx<0)
    throw OutOfBoundsException();

  assertArray();
  if ((size_t)idx >= std::get<ValueArray>(value).size())
    throw OutOfBoundsException();

  return std::get<ValueArray>(value)[idx];
}

AmArg& AmArg::operator[](std::string key) {
  assertStruct();
  return std::get<ValueStruct>(value)[key];
}

const AmArg& AmArg::operator[](std::string key) const {
  assertStruct();
  return std::get<ValueStruct>(value).at(key);
}

AmArg& AmArg::operator[](const char* key) {
  assertStruct();
  return std::get<ValueStruct>(value)[key];
}

const AmArg& AmArg::operator[](const char* key) const {
  assertStruct();
  return std::get<ValueStruct>(value).at(key);
}

bool AmArg::operator==(const char *val) const {
  return type == CStr && std::get<std::string>(value) == val;
}

bool AmArg::hasMember(const char* name) const {
  return type == Struct && std::get<ValueStruct>(value).find(name) != std::get<ValueStruct>(value).end();
}

bool AmArg::hasMember(const string& name) const {
  return type == Struct && std::get<ValueStruct>(value).find(name) != std::get<ValueStruct>(value).end();
}

std::vector<std::string> AmArg::enumerateKeys() const {
  assertStruct();
  std::vector<std::string> res;
  for (ValueStruct::const_iterator it =
	 std::get<ValueStruct>(value).begin(); it != std::get<ValueStruct>(value).end(); it++)
    res.push_back(it->first);
  return res;
}

AmArg::ValueStruct::const_iterator AmArg::begin() const {
  assertStruct();
  return std::get<ValueStruct>(value).begin();
}

AmArg::ValueStruct::const_iterator AmArg::end() const {
  assertStruct();
  return std::get<ValueStruct>(value).end();
}

void AmArg::erase(const char* name) {
  assertStruct();
  std::get<ValueStruct>(value).erase(name);
}

void AmArg::erase(const std::string& name) {
  assertStruct();
  std::get<ValueStruct>(value).erase(name);
}

void AmArg::assertArrayFmt(const char* format) const {
  size_t fmt_len = strlen(format);
  string got;
  try {
    for (size_t i=0;i<fmt_len;i++) {
      switch (format[i]) {
      case 'i': assertArgInt(get(i)); got+='i';  break;
      case 'l': assertArgLongLong(get(i)); got+='l';  break;
      case 't': assertArgBool(get(i)); got+='t';  break;
      case 'f': assertArgDouble(get(i)); got+='f'; break;
      case 's': assertArgCStr(get(i)); got+='s'; break;
      case 'o': assertArgAObject(get(i)); got+='o'; break;
      case 'd': assertArgADynInv(get(i)); got+='d'; break;
      case 'a': assertArgArray(get(i)); got+='a'; break;
      case 'b': assertArgBlob(get(i)); got+='b'; break;
      case 'u': assertArgStruct(get(i)); got+='u'; break;
      default: got+='?'; ERROR("ignoring unknown format type '%c'\n",
			       format[i]); break;
      }
    }
  } catch (...) {
    ERROR("parameter mismatch: expected '%s', got '%s...'\n",
	  format, got.c_str());
    throw;
  }
}

#define VECTOR_GETTER(type, name, getter)	\
  vector<type> AmArg::name() const {		\
    vector<type> res;				\
    for (size_t i=0;i<size();i++)		\
      res.push_back(get(i).getter());		\
    return res;					\
  }
VECTOR_GETTER(string, asStringVector, asCStr)
VECTOR_GETTER(int, asIntVector, asInt)
VECTOR_GETTER(bool, asBoolVector, asBool)
VECTOR_GETTER(double, asDoubleVector, asDouble)
VECTOR_GETTER(AmObject*, asAmObjectVector, asObject)
#undef  VECTOR_GETTER

vector<ArgBlob> AmArg::asArgBlobVector() const {
  vector<ArgBlob> res;
  for (size_t i=0;i<size();i++)
    res.push_back(get(i).asBlob());
  return res;
}

void AmArg::clear() {
  invalidate();
}

string AmArg::print(const AmArg &a) {
  string s;
  switch (a.getType()) {
    case Undef:
      return "";
    case Int:
      return a.asInt()<0?"-"+int2str(abs(a.asInt())):int2str(abs(a.asInt()));
    case LongLong:
      return longlong2str(a.asLongLong());
    case Bool:
      return a.asBool()?"true":"false";
    case Double:
      return double2str(a.asDouble());
    case CStr:
      return "'" + string(a.asCStr()) + "'";
    case AObject:
      return "<Object>";
    case ADynInv:
      return "<DynInv>";
    case Blob:
      s = "<Blob of size:" + int2str(a.asBlob().len) + ">";
      return s;
    case Array:
      s = "[";
      for (size_t i = 0; i < a.size(); i ++)
        s += print(a[i]) + ", ";
      if (1 < s.size())
        s.resize(s.size() - 2); // strip last ", "
      s += "]";
      return s;
    case Struct:
      s = "{";
      for (AmArg::ValueStruct::const_iterator it = a.asStruct().begin();
          it != a.asStruct().end(); it ++) {
        s += "'"+it->first + "': ";
        s += print(it->second);
        s += ", ";
      }
      if (1 < s.size())
        s.resize(s.size() - 2); // strip last ", "
      s += "}";
      return s;
    default: break;
  }
  return "<UNKONWN TYPE>";
}

const int arg2int(const AmArg &a)
{
  if (isArgInt(a)) return a.asInt();
  if (isArgBool(a)) return a.asBool();
  if (isArgCStr(a)) {
    int res;
    if (!str2int(a.asCStr(), res)) {
      throw std::string("can't convert arg to int: " + string(a.asCStr()));
    }
    return res;
  }

  throw std::string("can't convert arg to int");
}

string arg2str(const AmArg &a)
{
  if (isArgUndef(a)) return "";
  if (isArgInt(a)) return int2str(a.asInt());
  if (isArgBool(a)) return int2str(a.asBool());
  if (isArgCStr(a)) return a.asCStr();

  throw std::string("can't convert arg to string");
}

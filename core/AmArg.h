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

#ifndef _AmArg_h_
#define _AmArg_h_

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include <vector>
using std::vector;

#include <string>
using std::string;

#include <map>
#include <variant>

#include "log.h"

/** base for Objects as @see AmArg parameter, not owned by AmArg (!) */
class AmObject {
 public:
  AmObject() { }
  virtual ~AmObject() { }
};

struct ArgBlob {

  void* data;
  int   len;

  ArgBlob()
  : data(NULL),len(0)
  {
  }

  ArgBlob(const ArgBlob& a) {
    len = a.len;
    data = malloc(len);
    if (data)
      memcpy(data, a.data, len);
  }

  ArgBlob(const void* _data, int _len) {
    len = _len;
    data = malloc(len);
    if (data)
      memcpy(data, _data, len);
  }

  ~ArgBlob() { if (data) free(data); }
};

class AmDynInvoke;

/** \brief variable type argument for DynInvoke APIs */
class AmArg
: public AmObject
{
 public:
  // type enum
  enum {
    Undef=0,

    Int,
    LongLong,
    Bool,
    Double,
    CStr,
    AObject, // pointer to an object not owned by AmArg
    ADynInv, // pointer to a AmDynInvoke (useful for call backs)
    Blob,

    Array,
    Struct
  };

  struct OutOfBoundsException {
    OutOfBoundsException() { }
  };

  struct TypeMismatchException {
    TypeMismatchException() { }
  };

  typedef std::vector<AmArg> ValueArray;
  typedef std::map<std::string, AmArg> ValueStruct;

 private:
  // type
  short type;

  // value
  std::variant<std::monostate, long int, long long int, bool, double, std::string, AmObject*,
    AmDynInvoke*, ArgBlob, ValueArray*, ValueStruct*> value;

  void invalidate();

 public:

 AmArg()
   : type(Undef)
  { }

 AmArg(const AmArg& v);

 AmArg(const int& v)
   : type(Int),
    value(static_cast<long>(v))
    { }

 AmArg(const long int& v)
   : type(Int),
    value(v)
    { }

 AmArg(const long long int& v)
   : type(LongLong),
    value(v)
    { }

 AmArg(const bool& v)
   : type(Bool),
    value(v)
    { }

 AmArg(const double& v)
   : type(Double),
    value(v)
    { }

 AmArg(const char* v)
   : type(CStr),
    value(v)
    { }

 AmArg(const string &v)
   : type(CStr),
    value(v)
    { }

 AmArg(const ArgBlob& v)
   : type(Blob),
    value(v)
    { }

  AmArg(AmObject* v)
   : type(AObject),
    value(v)
    { }

  AmArg(AmDynInvoke* v)
   : type(ADynInv),
    value(v)
    { }

  // convenience constructors
  AmArg(vector<std::string>& v);
  AmArg(const vector<int>& v );
  AmArg(const vector<double>& v);
  AmArg(std::map<std::string, std::string>& v);
  AmArg(std::map<std::string, AmArg>& v);

  ~AmArg() { invalidate(); }

  void assertArray();
  void assertArray() const;

  void assertStruct();
  void assertStruct() const;

  short getType() const { return type; }

  AmArg& operator=(const AmArg& rhs);

#define isArgUndef(a) (AmArg::Undef == a.getType())
#define isArgArray(a) (AmArg::Array == a.getType())
#define isArgStruct(a)(AmArg::Struct == a.getType())
#define isArgDouble(a) (AmArg::Double == a.getType())
#define isArgInt(a) (AmArg::Int == a.getType())
#define isArgLongLong(a) (AmArg::LongLong == a.getType())
#define isArgBool(a) (AmArg::Bool == a.getType())
#define isArgCStr(a) (AmArg::CStr == a.getType())
#define isArgAObject(a) (AmArg::AObject == a.getType())
#define isArgADynInv(a) (AmArg::ADynInv == a.getType())
#define isArgBlob(a) (AmArg::Blob == a.getType())

#define _THROW_TYPE_MISMATCH(exp,got) \
	do { \
		ERROR("type mismatch: expected: %d; received: %d.\n", AmArg::exp, got.getType()); \
		throw AmArg::TypeMismatchException(); \
	} while (0)

#define assertArgArray(a)			\
  if (!isArgArray(a))				\
	_THROW_TYPE_MISMATCH(Array,a);
#define assertArgDouble(a)			\
  if (!isArgDouble(a))				\
	_THROW_TYPE_MISMATCH(Double,a);
#define assertArgInt(a)				\
  if (!isArgInt(a))				\
	_THROW_TYPE_MISMATCH(Int,a);
#define assertArgLongLong(a)				\
  if (!isArgLongLong(a))				\
	_THROW_TYPE_MISMATCH(LongLong,a);
#define assertArgBool(a)				\
  if (!isArgBool(a))				\
	_THROW_TYPE_MISMATCH(Bool,a);
#define assertArgCStr(a)			\
  if (!isArgCStr(a))				\
	_THROW_TYPE_MISMATCH(CStr,a);
#define assertArgAObject(a)			\
  if (!isArgAObject(a))				\
	_THROW_TYPE_MISMATCH(AObject,a);
#define assertArgADynInv(a)			\
  if (!isArgADynInv(a))				\
	_THROW_TYPE_MISMATCH(ADynInv,a);
#define assertArgBlob(a)			\
  if (!isArgBlob(a))				\
	_THROW_TYPE_MISMATCH(Blob,a);
#define assertArgStruct(a)			\
  if (!isArgStruct(a))				\
	_THROW_TYPE_MISMATCH(Struct,a);

  void setBorrowedPointer(AmObject* v) {
    invalidate();
    type = AObject;
    value = v;
  }

  int             asInt()      const { return std::get<long>(value); }
  long int        asLong()     const { return std::get<long>(value); }
  long long       asLongLong() const { return std::get<long long>(value); }
  bool            asBool()     const { return std::get<bool>(value); }
  double          asDouble()   const { return std::get<double>(value); }
  const char*     asCStr()     const { return std::get<std::string>(value).c_str(); }
  AmObject*       asObject()   const { return std::get<AmObject*>(value); }
  AmDynInvoke*    asDynInv()   const { return std::get<AmDynInvoke*>(value); }
  const ArgBlob&  asBlob()     const { return std::get<ArgBlob>(value); }
  ValueStruct*    asStruct()   const { return std::get<ValueStruct*>(value); }

  vector<string>     asStringVector()   const;
  vector<int>        asIntVector()      const;
  vector<bool>       asBoolVector()     const;
  vector<double>     asDoubleVector()   const;
  vector<AmObject*> asAmObjectVector()  const;
  vector<ArgBlob>    asArgBlobVector()  const;

  // operations on arrays
  void assertArray(size_t s);

  void push(const AmArg& a);
  void push(const string &key, const AmArg &val);
  void pop(AmArg &a);
  void pop_back(AmArg &a);
  void pop_back();

  void concat(const AmArg& a);

  size_t size() const;

  /** throws OutOfBoundsException if array too small */
  AmArg& get(size_t idx);

  /** throws OutOfBoundsException if array too small */
  AmArg& get(size_t idx) const;

  /** throws OutOfBoundsException if array too small */
  AmArg& back();

  /** throws OutOfBoundsException if array too small */
  AmArg& back() const;

  /** resizes array if too small */
  AmArg& operator[](size_t idx);
  /** throws OutOfBoundsException if array too small */
  AmArg& operator[](size_t idx) const;

  /** resizes array if too small */
  AmArg& operator[](int idx);
  /** throws OutOfBoundsException if array too small */
  AmArg& operator[](int idx) const;

  AmArg& operator[](std::string key);
  AmArg& operator[](std::string key) const;
  AmArg& operator[](const char* key);
  AmArg& operator[](const char* key) const;

  bool operator==(const char *) const;

  /** Check for the existence of a struct member by name. */
  bool hasMember(const std::string& name) const;
  bool hasMember(const char* name) const;

  std::vector<std::string> enumerateKeys() const;
  ValueStruct::const_iterator begin() const;
  ValueStruct::const_iterator end() const;

  /** remove struct member */
  void erase(const char* name);
  /** remove struct member */
  void erase(const std::string& name);

  /**
   * throws exception if arg array does not conform to spec
   *   i  - int
   *   l  - long long
   *   t  - bool
   *   f  - double
   *   s  - cstr
   *   o  - object
   *   d  - dyninvoke
   *   b  - blob
   *   a  - array
   *   u  - struct
   *
   *   e.g. "ssif" -> [cstr, cstr, int, double]
   */
  void assertArrayFmt(const char* format) const;

  void clear();

  friend bool json2arg(std::istream& input, AmArg& res);

  static string print(const AmArg &a);

  static const char* t2str(int type);
};

const int arg2int(const AmArg &a);
string arg2str(const AmArg &a);

#endif


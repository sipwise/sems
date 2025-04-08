
#include "XmlRpcValue.h"
#include "XmlRpcException.h"
#include "XmlRpcUtil.h"
#include "base64.h"

#ifndef MAKEDEPEND
# include <iostream>
# include <ostream>
# include <stdlib.h>
# include <stdio.h>
#endif


namespace XmlRpc {


  static const char VALUE_TAG[]     = "<value>";
  static const char VALUE_ETAG[]    = "</value>";

  static const char BOOLEAN_TAG[]   = "<boolean>";
  static const char BOOLEAN_ETAG[]  = "</boolean>";
  static const char DOUBLE_TAG[]    = "<double>";
  static const char DOUBLE_ETAG[]   = "</double>";
  static const char INT_TAG[]       = "<int>";
  static const char I4_TAG[]        = "<i4>";
  static const char I4_ETAG[]       = "</i4>";
  static const char STRING_TAG[]    = "<string>";
  static const char DATETIME_TAG[]  = "<dateTime.iso8601>";
  static const char DATETIME_ETAG[] = "</dateTime.iso8601>";
  static const char BASE64_TAG[]    = "<base64>";
  static const char BASE64_ETAG[]   = "</base64>";

  static const char ARRAY_TAG[]     = "<array>";
  static const char DATA_TAG[]      = "<data>";
  static const char DATA_ETAG[]     = "</data>";
  static const char ARRAY_ETAG[]    = "</array>";

  static const char STRUCT_TAG[]    = "<struct>";
  static const char MEMBER_TAG[]    = "<member>";
  static const char NAME_TAG[]      = "<name>";
  static const char NAME_ETAG[]     = "</name>";
  static const char MEMBER_ETAG[]   = "</member>";
  static const char STRUCT_ETAG[]   = "</struct>";



  // Format strings
  std::string XmlRpcValue::_doubleFormat("%f");



  // Clean up
  void XmlRpcValue::invalidate()
  {
    _value = std::monostate();
  }


  // Type checking
  void XmlRpcValue::assertTypeOrInvalid(Type t)
  {
    if (_type == TypeInvalid)
    {
      _type = t;
      switch (_type) {    // Ensure there is a valid value for the type
        case TypeString:   _value = std::string();     break;
        case TypeDateTime: _value = tm();              break;
        case TypeBase64:   _value = BinaryData();      break;
        case TypeArray:    _value = ValueArray();      break;
        case TypeStruct:   _value = ValueStruct();     break;
        case TypeInt:      _value = 0;                 break;
        case TypeDouble:   _value = 0.0;               break;
        case TypeBoolean:  _value = false;             break;
	case TypeInvalid:  _value = std::monostate();  break;
      }
    }
    else if (_type != t)
      throw XmlRpcException("type error");
  }

  void XmlRpcValue::assertArray(size_t size) const
  {
    if (_type != TypeArray)
      throw XmlRpcException("type error: expected an array");
    else if (std::get<ValueArray>(_value).size() < size)
      throw XmlRpcException("range error: array index too large");
  }


  void XmlRpcValue::assertArray(size_t size)
  {
    if (_type == TypeInvalid) {
      _type = TypeArray;
      _value = ValueArray(size);
    } else if (_type == TypeArray) {
      if (std::get<ValueArray>(_value).size() < size)
        std::get<ValueArray>(_value).resize(size);
    } else
      throw XmlRpcException("type error: expected an array");
  }

  void XmlRpcValue::assertStruct()
  {
    if (_type == TypeInvalid) {
      _type = TypeStruct;
      _value = ValueStruct();
    } else if (_type != TypeStruct)
      throw XmlRpcException("type error: expected a struct");
  }


  // Works for strings, binary data, arrays, and structs.
  size_t XmlRpcValue::size() const
  {
    switch (_type) {
      case TypeString: return std::get<std::string>(_value).size();
      case TypeBase64: return std::get<BinaryData>(_value).size();
      case TypeArray:  return std::get<ValueArray>(_value).size();
      case TypeStruct: return std::get<ValueStruct>(_value).size();
      default: break;
    }

    throw XmlRpcException("type error");
  }

  // Checks for existence of struct member
  bool XmlRpcValue::hasMember(const std::string& name) const
  {
    return _type == TypeStruct && std::get<ValueStruct>(_value).find(name) != std::get<ValueStruct>(_value).end();
  }

  // Set the value from xml. The chars at *offset into valueXml
  // should be the start of a <value> tag. Destroys any existing value.
  bool XmlRpcValue::fromXml(std::string const& valueXml, size_t* offset)
  {
    size_t savedOffset = *offset;

    invalidate();
    if ( ! XmlRpcUtil::nextTagIs(VALUE_TAG, valueXml, offset))
      return false;       // Not a value, offset not updated

	size_t afterValueOffset = *offset;
    std::string typeTag = XmlRpcUtil::getNextTag(valueXml, offset);
    bool result = false;
    if (typeTag == BOOLEAN_TAG)
      result = boolFromXml(valueXml, offset);
    else if (typeTag == I4_TAG || typeTag == INT_TAG)
      result = intFromXml(valueXml, offset);
    else if (typeTag == DOUBLE_TAG)
      result = doubleFromXml(valueXml, offset);
    else if (typeTag.empty() || typeTag == STRING_TAG)
      result = stringFromXml(valueXml, offset);
    else if (typeTag == DATETIME_TAG)
      result = timeFromXml(valueXml, offset);
    else if (typeTag == BASE64_TAG)
      result = binaryFromXml(valueXml, offset);
    else if (typeTag == ARRAY_TAG)
      result = arrayFromXml(valueXml, offset);
    else if (typeTag == STRUCT_TAG)
      result = structFromXml(valueXml, offset);
    // Watch for empty/blank strings with no <string>tag
    else if (typeTag == VALUE_ETAG)
    {
      *offset = afterValueOffset;   // back up & try again
      result = stringFromXml(valueXml, offset);
    }

    if (result)  // Skip over the </value> tag
      XmlRpcUtil::findTag(VALUE_ETAG, valueXml, offset);
    else        // Unrecognized tag after <value>
      *offset = savedOffset;

    return result;
  }

  // Encode the Value in xml
  std::string XmlRpcValue::toXml() const
  {
    switch (_type) {
      case TypeBoolean:  return boolToXml();
      case TypeInt:      return intToXml();
      case TypeDouble:   return doubleToXml();
      case TypeString:   return stringToXml();
      case TypeDateTime: return timeToXml();
      case TypeBase64:   return binaryToXml();
      case TypeArray:    return arrayToXml();
      case TypeStruct:   return structToXml();
      default: break;
    }
    return std::string();   // Invalid value
  }


  // Boolean
  bool XmlRpcValue::boolFromXml(std::string const& valueXml, size_t* offset)
  {
    const char* valueStart = valueXml.c_str() + *offset;
    char* valueEnd;
    long ivalue = strtol(valueStart, &valueEnd, 10);
    if (valueEnd == valueStart || (ivalue != 0 && ivalue != 1))
      return false;

    _type = TypeBoolean;
    _value = (ivalue == 1);
    *offset += valueEnd - valueStart;
    return true;
  }

  std::string XmlRpcValue::boolToXml() const
  {
    std::string xml = VALUE_TAG;
    xml += BOOLEAN_TAG;
    xml += (std::get<bool>(_value) ? "1" : "0");
    xml += BOOLEAN_ETAG;
    xml += VALUE_ETAG;
    return xml;
  }

  // Int
  bool XmlRpcValue::intFromXml(std::string const& valueXml, size_t* offset)
  {
    const char* valueStart = valueXml.c_str() + *offset;
    char* valueEnd;
    long ivalue = strtol(valueStart, &valueEnd, 10);
    if (valueEnd == valueStart)
      return false;

    _type = TypeInt;
    _value = ivalue;
    *offset += valueEnd - valueStart;
    return true;
  }

  std::string XmlRpcValue::intToXml() const
  {
    char buf[256];
    snprintf(buf, sizeof(buf)-1, "%ld", std::get<long>(_value));
    buf[sizeof(buf)-1] = 0;
    std::string xml = VALUE_TAG;
    xml += I4_TAG;
    xml += buf;
    xml += I4_ETAG;
    xml += VALUE_ETAG;
    return xml;
  }

  // Double
  bool XmlRpcValue::doubleFromXml(std::string const& valueXml, size_t* offset)
  {
    const char* valueStart = valueXml.c_str() + *offset;
    char* valueEnd;
    double dvalue = strtod(valueStart, &valueEnd);
    if (valueEnd == valueStart)
      return false;

    _type = TypeDouble;
    _value = dvalue;
    *offset += valueEnd - valueStart;
    return true;
  }

  std::string XmlRpcValue::doubleToXml() const
  {
    char buf[256];
    snprintf(buf, sizeof(buf)-1, getDoubleFormat().c_str(), std::get<double>(_value));
    buf[sizeof(buf)-1] = 0;

    std::string xml = VALUE_TAG;
    xml += DOUBLE_TAG;
    xml += buf;
    xml += DOUBLE_ETAG;
    xml += VALUE_ETAG;
    return xml;
  }

  // String
  bool XmlRpcValue::stringFromXml(std::string const& valueXml, size_t* offset)
  {
    size_t valueEnd = valueXml.find('<', *offset);
    if (valueEnd == std::string::npos)
      return false;     // No end tag;

    _type = TypeString;
    _value.emplace<std::string>(XmlRpcUtil::xmlDecode(valueXml.substr(*offset, valueEnd-*offset)));
    *offset += std::get<std::string>(_value).length();
    return true;
  }

  std::string XmlRpcValue::stringToXml() const
  {
    std::string xml = VALUE_TAG;
    //xml += STRING_TAG; optional
    xml += XmlRpcUtil::xmlEncode(std::get<std::string>(_value));
    //xml += STRING_ETAG;
    xml += VALUE_ETAG;
    return xml;
  }

  // DateTime (stored as a struct tm)
  bool XmlRpcValue::timeFromXml(std::string const& valueXml, size_t* offset)
  {
    size_t valueEnd = valueXml.find('<', *offset);
    if (valueEnd == std::string::npos)
      return false;     // No end tag;

    std::string stime = valueXml.substr(*offset, valueEnd-*offset);

    /* zero-initialize */
    struct tm t = {};
    if (sscanf(stime.c_str(),"%4d%2d%2dT%2d:%2d:%2d",&t.tm_year,&t.tm_mon,&t.tm_mday,&t.tm_hour,&t.tm_min,&t.tm_sec) != 6)
      return false;

    t.tm_year -= 1900;
    t.tm_isdst = -1;
    t.tm_mon--; /* sscanf reads months as 1-12, but tm_mon expects 0-11. So not adjusting it can lead to an incorrect date. */

    _type = TypeDateTime;
    _value = t;
    *offset += stime.length();
    return true;
  }

  std::string XmlRpcValue::timeToXml() const
  {
    const tm& t = std::get<tm>(_value);
    char buf[18];

    if (snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d:%02d:%02d",
                (1900 + t.tm_year), (1 + t.tm_mon), t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec) < 0)
    {
      XmlRpcUtil::log(2,"timeToXml: issues while trying to write data.");
    }

    std::string xml = VALUE_TAG;
    xml += DATETIME_TAG;
    xml += buf;
    xml += DATETIME_ETAG;
    xml += VALUE_ETAG;
    return xml;
  }


  // Base64
  bool XmlRpcValue::binaryFromXml(std::string const& valueXml, size_t* offset)
  {
    size_t valueEnd = valueXml.find('<', *offset);
    if (valueEnd == std::string::npos)
      return false;     // No end tag;

    _type = TypeBase64;
    std::string asString = valueXml.substr(*offset, valueEnd-*offset);
    _value.emplace<BinaryData>();
    // check whether base64 encodings can contain chars xml encodes...

    // convert from base64 to binary
    int iostatus = 0;
	  base64<char> decoder;
    std::back_insert_iterator<BinaryData> ins = std::back_inserter(std::get<BinaryData>(_value));
		decoder.get(asString.begin(), asString.end(), ins, iostatus);

    *offset += asString.length();
    return true;
  }


  std::string XmlRpcValue::binaryToXml() const
  {
    // convert to base64
    std::vector<char> base64data;
    int iostatus = 0;
	  base64<char> encoder;
    std::back_insert_iterator<std::vector<char> > ins = std::back_inserter(base64data);
		encoder.put(std::get<BinaryData>(_value).begin(), std::get<BinaryData>(_value).end(), ins, iostatus, base64<>::crlf());

    // Wrap with xml
    std::string xml = VALUE_TAG;
    xml += BASE64_TAG;
    xml.append(base64data.begin(), base64data.end());
    xml += BASE64_ETAG;
    xml += VALUE_ETAG;
    return xml;
  }


  // Array
  bool XmlRpcValue::arrayFromXml(std::string const& valueXml, size_t* offset)
  {
    if ( ! XmlRpcUtil::nextTagIs(DATA_TAG, valueXml, offset))
      return false;

    _type = TypeArray;
    _value.emplace<ValueArray>();
    XmlRpcValue v;
    while (v.fromXml(valueXml, offset))
      std::get<ValueArray>(_value).push_back(v);       // copy...

    // Skip the trailing </data>
    (void) XmlRpcUtil::nextTagIs(DATA_ETAG, valueXml, offset);
    return true;
  }


  // In general, its preferable to generate the xml of each element of the
  // array as it is needed rather than glomming up one big string.
  std::string XmlRpcValue::arrayToXml() const
  {
    std::string xml = VALUE_TAG;
    xml += ARRAY_TAG;
    xml += DATA_TAG;

    size_t s = std::get<ValueArray>(_value).size();
    for (size_t i=0; i<s; ++i)
       xml += std::get<ValueArray>(_value).at(i).toXml();

    xml += DATA_ETAG;
    xml += ARRAY_ETAG;
    xml += VALUE_ETAG;
    return xml;
  }


  // Struct
  bool XmlRpcValue::structFromXml(std::string const& valueXml, size_t* offset)
  {
    _type = TypeStruct;
    _value.emplace<ValueStruct>();

    while (XmlRpcUtil::nextTagIs(MEMBER_TAG, valueXml, offset)) {
      // name
      const std::string name = XmlRpcUtil::parseTag(NAME_TAG, valueXml, offset);
      // value
      XmlRpcValue val(valueXml, offset);
      if ( ! val.valid()) {
        invalidate();
        return false;
      }
      const std::pair<const std::string, XmlRpcValue> p(std::move(name), std::move(val));
      std::get<ValueStruct>(_value).insert(p);

      (void) XmlRpcUtil::nextTagIs(MEMBER_ETAG, valueXml, offset);
    }
    return true;
  }


  // In general, its preferable to generate the xml of each element
  // as it is needed rather than glomming up one big string.
  std::string XmlRpcValue::structToXml() const
  {
    std::string xml = VALUE_TAG;
    xml += STRUCT_TAG;

    ValueStruct::const_iterator it;
    for (it=std::get<ValueStruct>(_value).begin(); it!=std::get<ValueStruct>(_value).end(); ++it) {
      xml += MEMBER_TAG;
      xml += NAME_TAG;
      xml += XmlRpcUtil::xmlEncode(it->first);
      xml += NAME_ETAG;
      xml += it->second.toXml();
      xml += MEMBER_ETAG;
    }

    xml += STRUCT_ETAG;
    xml += VALUE_ETAG;
    return xml;
  }



  // Write the value without xml encoding it
  std::ostream& XmlRpcValue::write(std::ostream& os) const {
    switch (_type) {
      default:           break;
      case TypeBoolean:  os << std::get<bool>(_value);        break;
      case TypeInt:      os << std::get<long>(_value);        break;
      case TypeDouble:   os << std::get<double>(_value);      break;
      case TypeString:   os << std::get<std::string>(_value); break;
      case TypeDateTime:
        {
          const tm& t = std::get<tm>(_value);
          char buf[20];
          snprintf(buf, sizeof(buf)-1, "%4d%02d%02dT%02d:%02d:%02d",
            (1900 + t.tm_year), (1 + t.tm_mon), t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
          buf[sizeof(buf)-1] = 0;
          os << buf;
          break;
        }
      case TypeBase64:
        {
          int iostatus = 0;
          std::ostreambuf_iterator<char> out(os);
          base64<char> encoder;
          encoder.put(std::get<BinaryData>(_value).begin(), std::get<BinaryData>(_value).end(), out, iostatus, base64<>::crlf());
          break;
        }
      case TypeArray:
        {
          size_t s = std::get<ValueArray>(_value).size();
          os << '{';
          for (size_t i=0; i<s; ++i)
          {
            if (i > 0) os << ',';
	    std::get<ValueArray>(_value).at(i).write(os);
          }
          os << '}';
          break;
        }
      case TypeStruct:
        {
          os << '[';
          ValueStruct::const_iterator it;
          for (it=std::get<ValueStruct>(_value).begin(); it!=std::get<ValueStruct>(_value).end(); ++it)
          {
            if (it!=std::get<ValueStruct>(_value).begin()) os << ',';
            os << it->first << ':';
            it->second.write(os);
          }
          os << ']';
          break;
        }

    }

    return os;
  }

} // namespace XmlRpc


// ostream
std::ostream& operator<<(std::ostream& os, XmlRpc::XmlRpcValue& v)
{
  // If you want to output in xml format:
  //return os << v.toXml();
  return v.write(os);
}


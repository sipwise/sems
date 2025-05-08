
#ifndef _XMLRPCVALUE_H_
#define _XMLRPCVALUE_H_
//
// XmlRpc++ Copyright (c) 2002-2003 by Chris Morley
//

#if defined(_MSC_VER)
# pragma warning(disable:4786)    // identifier was truncated in debug info
#endif

#ifndef MAKEDEPEND
# include <map>
# include <string>
# include <vector>
# include <time.h>
#endif

#include <variant>

namespace XmlRpc {

  //! A class to represent RPC arguments and results.
  //! Each XmlRpcValue object contains a typed value,
  //! where the type is determined by the initial value
  //! assigned to the object.
  //   should probably refcount them...
  class XmlRpcValue {
  public:

    //! XmlRpcValue types
    enum Type {
      TypeInvalid,
      TypeBoolean,
      TypeInt,  // really a long
      TypeDouble,
      TypeString,
      TypeDateTime,
      TypeBase64,
      TypeArray,
      TypeStruct
    };

    // Non-primitive types
    typedef std::vector<char> BinaryData;
    typedef std::vector<XmlRpcValue> ValueArray;
    typedef std::map<std::string, XmlRpcValue> ValueStruct;


    // Constructors
    //! Construct an empty XmlRpcValue
    XmlRpcValue() : _type(TypeInvalid) { }

    //! Construct an XmlRpcValue with a bool value
    XmlRpcValue(bool value) : _type(TypeBoolean) { _value = value; }

    //! Construct an XmlRpcValue with a long value
    XmlRpcValue(long value)  : _type(TypeInt) { _value = value; }
    XmlRpcValue(int value)   : _type(TypeInt) { _value = value; }

    //! Construct an XmlRpcValue with a double value
    XmlRpcValue(double value)  : _type(TypeDouble) { _value = value; }

    //! Construct an XmlRpcValue with a string value
    XmlRpcValue(std::string const& value) : _type(TypeString)
    { _value = value; }

    //! Construct an XmlRpcValue with a string value.
    //! @param value A null-terminated (C) string.
    XmlRpcValue(const char* value)  : _type(TypeString)
    { _value.emplace<std::string>(value); }

    //! Construct an XmlRpcValue with a date/time value.
    //! @param value A pointer to a struct tm (see localtime)
    XmlRpcValue(const tm& value)  : _type(TypeDateTime)
    { _value = value; }

    //! Construct an XmlRpcValue with a binary data value
    //! @param value A pointer to data
    //! @param nBytes The length of the data pointed to, in bytes
    XmlRpcValue(const void* value, size_t nBytes)  : _type(TypeBase64)
    {
      _value.emplace<BinaryData>((char*)value, ((char*)value)+nBytes);
    }

    //! Construct from xml, beginning at *offset chars into the string, updates offset
    XmlRpcValue(std::string const& xml, size_t* offset) : _type(TypeInvalid)
    { if ( ! fromXml(xml,offset)) _type = TypeInvalid; }

    //! Copy constructor
    XmlRpcValue(XmlRpcValue const& rhs) : _type(TypeInvalid) { *this = rhs; }

    //! Destructor (make virtual if you want to subclass)
    /*virtual*/ ~XmlRpcValue() { invalidate(); }

    //! Erase the current value
    void clear() { invalidate(); }

    // Operators
    //! Assignment from one XmlRpcValue to this one.
    //! @param rhs The value in rhs is copied to this value.
    XmlRpcValue& operator=(XmlRpcValue const& rhs) = default;

    //! Assign a long to this XmlRpcValue.
    XmlRpcValue& operator=(long const& rhs) { return operator=(XmlRpcValue(rhs)); }
    XmlRpcValue& operator=(int const& rhs)  { return operator=(XmlRpcValue(rhs)); }

    //! Assign a double to this XmlRpcValue.
    XmlRpcValue& operator=(double const& rhs) { return operator=(XmlRpcValue(rhs)); }

    /* Assign a bool to this XmlRpcValue. */
    XmlRpcValue& operator=(bool const& rhs) { return operator=(XmlRpcValue(rhs)); }

    //! Assign a string to this XmlRpcValue.
    XmlRpcValue& operator=(const char* rhs) { return operator=(XmlRpcValue(std::string(rhs))); }

    //! Tests two XmlRpcValues for equality
    bool operator==(XmlRpcValue const& other) const = default;

    //! Tests two XmlRpcValues for inequality
    bool operator!=(XmlRpcValue const& other) const = default;

    //! Treat an XmlRpcValue as a bool.
    //! Throws XmlRpcException if the value is initialized to
    //! a type that is not TypeBoolean.
    operator bool&()          { assertTypeOrInvalid(TypeBoolean); return std::get<bool>(_value); }

    //! Treat an XmlRpcValue as an int.
    //! Throws XmlRpcException if the value is initialized to
    //! a type that is not TypeInt.
    operator long&()           { assertTypeOrInvalid(TypeInt); return std::get<long>(_value); }

    //! Treat an XmlRpcValue as a double.
    //! Throws XmlRpcException if the value is initialized to
    //! a type that is not TypeDouble.
    operator double&()        { assertTypeOrInvalid(TypeDouble); return std::get<double>(_value); }

    //! Treat an XmlRpcValue as a string.
    //! Throws XmlRpcException if the value is initialized to
    //! a type that is not TypeString.
    operator std::string&()   { assertTypeOrInvalid(TypeString); return std::get<std::string>(_value); }

    //! Access the BinaryData value.
    //! Throws XmlRpcException if the value is initialized to
    //! a type that is not TypeBase64.
    operator BinaryData&()    { assertTypeOrInvalid(TypeBase64); return std::get<BinaryData>(_value); }

    //! Access the DateTime value.
    //! Throws XmlRpcException if the value is initialized to
    //! a type that is not TypeDateTime.
    operator struct tm&()     { assertTypeOrInvalid(TypeDateTime); return std::get<tm>(_value); }


    //! Const array value accessor.
    //! Access the ith value of the array.
    //! Throws XmlRpcException if the value is not an array or if the index i is
    //! not a valid index for the array.
    XmlRpcValue const& operator[](size_t i) const   { assertArray(i+1); return std::get<ValueArray>(_value).at(i); }
    XmlRpcValue const& operator[](int i) const      { assertArray(i+1); return std::get<ValueArray>(_value).at(i); }

    //! Array value accessor.
    //! Access the ith value of the array, growing the array if necessary.
    //! Throws XmlRpcException if the value is not an array.
    XmlRpcValue& operator[](size_t i)               { assertArray(i+1); return std::get<ValueArray>(_value).at(i); }
    XmlRpcValue& operator[](int i)                  { assertArray(i+1); return std::get<ValueArray>(_value).at(i); }

    //! Struct entry accessor.
    //! Returns the value associated with the given entry, creating one if necessary.
    XmlRpcValue& operator[](std::string const& k) { assertStruct(); return std::get<ValueStruct>(_value)[k]; }

    //! Struct entry accessor.
    //! Returns the value associated with the given entry, creating one if necessary.
    XmlRpcValue& operator[](const char* k) { assertStruct(); std::string s(k); return std::get<ValueStruct>(_value)[s]; }

    //! Access the struct value map.
    //! Can be used to iterate over the entries in the map to find all defined entries.
    operator ValueStruct const&() { assertStruct(); return std::get<ValueStruct>(_value); }

    // Accessors
    //! Return true if the value has been set to something.
    bool valid() const { return _type != TypeInvalid; }

    //! Return the type of the value stored. \see Type.
    Type const &getType() const { return _type; }

    //! Return the size for string, base64, array, and struct values.
    size_t size() const;

    //! Specify the size for array values. Array values will grow beyond this size if needed.
    void setSize(size_t size)    { assertArray(size); }

    //! Check for the existence of a struct member by name.
    bool hasMember(const std::string& name) const;

    //! Decode xml. Destroys any existing value.
    bool fromXml(std::string const& valueXml, size_t* offset);

    //! Encode the Value in xml
    std::string toXml() const;

    //! Write the value (no xml encoding)
    std::ostream& write(std::ostream& os) const;

    // Formatting
    //! Return the format used to write double values.
    static std::string const& getDoubleFormat() { return _doubleFormat; }

    //! Specify the format used to write double values.
    static void setDoubleFormat(const char* f) { _doubleFormat = f; }


  protected:
    // Clean up
    void invalidate();

    // Type checking
    void assertTypeOrInvalid(Type t);
    void assertArray(size_t size) const;
    void assertArray(size_t size);
    void assertStruct();

    // XML decoding
    bool boolFromXml(std::string const& valueXml, size_t* offset);
    bool intFromXml(std::string const& valueXml, size_t* offset);
    bool doubleFromXml(std::string const& valueXml, size_t* offset);
    bool stringFromXml(std::string const& valueXml, size_t* offset);
    bool timeFromXml(std::string const& valueXml, size_t* offset);
    bool binaryFromXml(std::string const& valueXml, size_t* offset);
    bool arrayFromXml(std::string const& valueXml, size_t* offset);
    bool structFromXml(std::string const& valueXml, size_t* offset);

    // XML encoding
    std::string boolToXml() const;
    std::string intToXml() const;
    std::string doubleToXml() const;
    std::string stringToXml() const;
    std::string timeToXml() const;
    std::string binaryToXml() const;
    std::string arrayToXml() const;
    std::string structToXml() const;

    // Format strings
    static std::string _doubleFormat;

    // Type tag and values
    Type _type;
    std::variant<std::monostate, bool, long, double, std::string, tm, BinaryData, ValueArray, ValueStruct> _value;

  };
} // namespace XmlRpc


std::ostream& operator<<(std::ostream& os, XmlRpc::XmlRpcValue& v);


#endif // _XMLRPCVALUE_H_

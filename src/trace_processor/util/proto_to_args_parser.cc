/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/util/proto_to_args_parser.h"

#include "protos/perfetto/common/descriptor.pbzero.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {
namespace util {

namespace {

// ScopedStringAppender will add |append| to |dest| when constructed and
// erases the appended suffix from |dest| when it goes out of scope. Thus
// |dest| must be valid for the entire lifetime of ScopedStringAppender.
//
// This is useful as we descend into a proto since the column names just
// appended with ".field_name" as we go lower.
//
// I.E. message1.message2.field_name1 is a column, but we'll then need to
// append message1.message2.field_name2 afterwards so we only need to append
// "field_name1" within some scope.
class ScopedStringAppender {
 public:
  ScopedStringAppender(const std::string& append, std::string* dest)
      : old_size_(dest->size()), dest_(dest) {
    if (dest->empty()) {
      dest_->reserve(append.size());
    } else {
      dest_->reserve(old_size_ + 1 + append.size());
      dest_->append(".");
    }
    dest_->append(append);
  }
  ~ScopedStringAppender() { dest_->erase(old_size_); }

 private:
  size_t old_size_;
  std::string* dest_;
};

}  // namespace

ProtoToArgsParser::Key::Key() = default;
ProtoToArgsParser::Key::Key(const std::string& k) : flat_key(k), key(k) {}
ProtoToArgsParser::Key::Key(const std::string& fk, const std::string& k)
    : flat_key(fk), key(k) {}
ProtoToArgsParser::Key::~Key() = default;

ProtoToArgsParser::Delegate::~Delegate() = default;

ProtoToArgsParser::ProtoToArgsParser(const DescriptorPool& pool) : pool_(pool) {
  constexpr int kDefaultSize = 64;
  key_prefix_.key.reserve(kDefaultSize);
  key_prefix_.flat_key.reserve(kDefaultSize);
}

base::Status ProtoToArgsParser::ParseMessage(
    const protozero::ConstBytes& cb,
    const std::string& type,
    const std::vector<uint16_t>* allowed_fields,
    Delegate& delegate) {
  auto idx = pool_.FindDescriptorIdx(type);
  if (!idx) {
    return base::Status("Failed to find proto descriptor");
  }

  auto& descriptor = pool_.descriptors()[*idx];

  std::unordered_map<size_t, int> repeated_field_index;

  protozero::ProtoDecoder decoder(cb);
  for (protozero::Field f = decoder.ReadField(); f.valid();
       f = decoder.ReadField()) {
    auto field = descriptor.FindFieldByTag(f.id());
    if (!field) {
      // Unknown field, possibly an unknown extension.
      continue;
    }

    // If allowlist is not provided, reflect all fields. Otherwise, check if the
    // current field either an extension or is in allowlist.
    bool is_allowed = field->is_extension() || !allowed_fields ||
                      std::find(allowed_fields->begin(), allowed_fields->end(),
                                f.id()) != allowed_fields->end();

    if (!is_allowed) {
      // Field is neither an extension, nor is allowed to be
      // reflected.
      continue;
    }
    RETURN_IF_ERROR(
        ParseField(*field, repeated_field_index[f.id()], f, delegate));
    if (field->is_repeated()) {
      repeated_field_index[f.id()]++;
    }
  }

  return base::OkStatus();
}

base::Status ProtoToArgsParser::ParseField(
    const FieldDescriptor& field_descriptor,
    int repeated_field_number,
    protozero::Field field,
    Delegate& delegate) {
  std::string prefix_part = field_descriptor.name();
  if (field_descriptor.is_repeated()) {
    std::string number = std::to_string(repeated_field_number);
    prefix_part.reserve(prefix_part.length() + number.length() + 2);
    prefix_part.append("[");
    prefix_part.append(number);
    prefix_part.append("]");
  }

  // In the args table we build up message1.message2.field1 as the column
  // name. This will append the ".field1" suffix to |key_prefix| and then
  // remove it when it goes out of scope.
  ScopedStringAppender scoped_prefix(prefix_part, &key_prefix_.key);
  ScopedStringAppender scoped_flat_key_prefix(field_descriptor.name(),
                                              &key_prefix_.flat_key);

  // If we have an override parser then use that instead and move onto the
  // next loop.
  if (base::Optional<base::Status> status =
          MaybeApplyOverride(field, delegate)) {
    return *status;
  }

  // If this is not a message we can just immediately add the column name and
  // get the value out of |field|. However if it is a message we need to
  // recurse into it.
  if (field_descriptor.type() ==
      protos::pbzero::FieldDescriptorProto::TYPE_MESSAGE) {
    return ParseMessage(field.as_bytes(), field_descriptor.resolved_type_name(),
                        nullptr, delegate);
  }

  return ParseSimpleField(field_descriptor, field, delegate);
}

void ProtoToArgsParser::AddParsingOverride(std::string field,
                                           ParsingOverride func) {
  overrides_[std::move(field)] = std::move(func);
}

base::Optional<base::Status> ProtoToArgsParser::MaybeApplyOverride(
    const protozero::Field& field,
    Delegate& delegate) {
  auto it = overrides_.find(key_prefix_.flat_key);
  if (it == overrides_.end())
    return base::nullopt;
  return it->second(field, delegate);
}

base::Status ProtoToArgsParser::ParseSimpleField(
    const FieldDescriptor& descriptor,
    const protozero::Field& field,
    Delegate& delegate) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  switch (descriptor.type()) {
    case FieldDescriptorProto::TYPE_INT32:
    case FieldDescriptorProto::TYPE_SFIXED32:
    case FieldDescriptorProto::TYPE_FIXED32:
      delegate.AddInteger(key_prefix_, field.as_int32());
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_SINT32:
      delegate.AddInteger(key_prefix_, field.as_sint32());
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_INT64:
    case FieldDescriptorProto::TYPE_SFIXED64:
    case FieldDescriptorProto::TYPE_FIXED64:
      delegate.AddInteger(key_prefix_, field.as_int64());
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_SINT64:
      delegate.AddInteger(key_prefix_, field.as_sint64());
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_UINT32:
      delegate.AddUnsignedInteger(key_prefix_, field.as_uint32());
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_UINT64:
      delegate.AddUnsignedInteger(key_prefix_, field.as_uint64());
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_BOOL:
      delegate.AddBoolean(key_prefix_, field.as_bool());
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_DOUBLE:
      delegate.AddDouble(key_prefix_, field.as_double());
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_FLOAT:
      delegate.AddDouble(key_prefix_, static_cast<double>(field.as_float()));
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_STRING:
      delegate.AddString(key_prefix_, field.as_string());
      return base::OkStatus();
    case FieldDescriptorProto::TYPE_ENUM: {
      auto opt_enum_descriptor_idx =
          pool_.FindDescriptorIdx(descriptor.resolved_type_name());
      if (!opt_enum_descriptor_idx) {
        delegate.AddInteger(key_prefix_, field.as_int32());
        return base::OkStatus();
      }
      auto opt_enum_string =
          pool_.descriptors()[*opt_enum_descriptor_idx].FindEnumString(
              field.as_int32());
      if (!opt_enum_string) {
        // Fall back to the integer representation of the field.
        delegate.AddInteger(key_prefix_, field.as_int32());
        return base::OkStatus();
      }
      delegate.AddString(key_prefix_,
                         protozero::ConstChars{opt_enum_string->data(),
                                               opt_enum_string->size()});
      return base::OkStatus();
    }
    default:
      return base::ErrStatus(
          "Tried to write value of type field %s (in proto type "
          "%s) which has type enum %d",
          descriptor.name().c_str(), descriptor.resolved_type_name().c_str(),
          descriptor.type());
  }
}

}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto

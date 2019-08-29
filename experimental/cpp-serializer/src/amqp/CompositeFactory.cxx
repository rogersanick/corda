#include "CompositeFactory.h"

#include <set>
#include <vector>
#include <iostream>
#include <algorithm>
#include <functional>

#include <assert.h>

#include "debug.h"

#include "amqp/reader/IReader.h"
#include "reader/Reader.h"
#include "amqp/reader/PropertyReader.h"
#include "reader/CompositeReader.h"
#include "reader/RestrictedReader.h"
#include "reader/restricted-readers/ListReader.h"

#include "schema/restricted-types/List.h"

/******************************************************************************/

namespace {

/**
 *
 */
    template<typename T>
    std::shared_ptr<T> &
    computeIfAbsent(
            spStrMap_t<T> &map_,
            const std::string &k_,
            std::function<std::shared_ptr<T>(void)> f_
    ) {
        auto it = map_.find(k_);

        if (it == map_.end()) {
            DBG ("ComputeIfAbsent \"" << k_ << "\" - missing" << std::endl); // NOLINT
            map_[k_] = std::move(f_());
            DBG ("                \"" << k_ << "\" - RTN: " << map_[k_]->name() << " : " << map_[k_]->type()
                                      << std::endl); // NOLINT
            assert (map_[k_]);
            assert (map_[k_] != nullptr);
            assert (k_ == map_[k_]->type());

            return map_[k_];
        } else {
            DBG ("ComputeIfAbsent \"" << k_ << "\" - found it" << std::endl); // NOLINT
            DBG ("                \"" << k_ << "\" - RTN: " << map_[k_]->name() << std::endl); // NOLINT

            assert (it->second != nullptr);

            return it->second;
        }
    }

}

/******************************************************************************
 *
 *  CompositeFactory
 *
 ******************************************************************************/

/**
 *
 * Walk through the types in a Schema and produce readers for them.
 *
 * We are making the large assumption that the contents of [schema_]
 * are strictly ordered by dependency so we can construct types
 * as we go without needing to provide look ahead for types
 * we haven't built yet
 *
 */
void
amqp::internal::
CompositeFactory::process (const SchemaType & schema_) {
    for (const auto & i : dynamic_cast<const schema::Schema &>(schema_)) {
        for (const auto & j : i) {
            process(*j);
            m_readersByDescriptor[j->descriptor()] = m_readersByType[j->name()];
        }
    }
}

/******************************************************************************/

std::shared_ptr<amqp::internal::reader::Reader>
amqp::internal::
CompositeFactory::process(
        const amqp::internal::schema::AMQPTypeNotation & schema_)
{
    return computeIfAbsent<reader::Reader> (
        m_readersByType,
        schema_.name(),
        [& schema_, this] () -> std::shared_ptr<reader::Reader> {
            switch (schema_.type()) {
                case amqp::internal::schema::AMQPTypeNotation::Composite : {
                    return processComposite(schema_);
                }
                case amqp::internal::schema::AMQPTypeNotation::Restricted : {
                    return processRestricted(schema_);
                }
            }
        });
}

/******************************************************************************/

std::shared_ptr<amqp::internal::reader::Reader>
amqp::internal::
CompositeFactory::processComposite (
        const amqp::internal::schema::AMQPTypeNotation & type_)
{
    std::vector<std::weak_ptr<reader::Reader>> readers;

    const auto & fields = dynamic_cast<const amqp::internal::schema::Composite &> (
            type_).fields();

    readers.reserve(fields.size());

    for (const auto & field : fields) {
        DBG ("  Field: " << field->name() << ": " << field->type() << std::endl); // NOLINT

        switch (field->fieldType()) {
            case schema::FieldType::PrimitiveProperty : {
                auto reader = computeIfAbsent<reader::Reader>(
                        m_readersByType,
                        field->type(),
                        [&field]() -> std::shared_ptr<reader::PropertyReader> {
                            return reader::PropertyReader::make(field);
                        });

                assert (reader);
                readers.emplace_back(reader);
                assert (readers.back().lock());
                break;
            }
            case amqp::internal::schema::FieldType::CompositeProperty : {
                auto reader = m_readersByType[field->type()];

                assert (reader);
                readers.emplace_back(reader);
                assert (readers.back().lock());
                break;
            }
            case schema::FieldType::RestrictedProperty :  {
                auto reader = m_readersByType[field->requires().front()];

                assert (reader);
                readers.emplace_back(reader);
                assert (readers.back().lock());
                break;
            }
        }

        assert (readers.back().lock());
    }

    return std::make_shared<reader::CompositeReader> (type_.name(), readers);
}

/******************************************************************************/

std::shared_ptr<amqp::internal::reader::Reader>
amqp::internal::
CompositeFactory::processRestricted (
        const amqp::internal::schema::AMQPTypeNotation & type_)
{
    DBG ("processRestricted - " << type_.name() << std::endl); // NOLINT
    const auto & restricted = dynamic_cast<const amqp::internal::schema::Restricted &> (
            type_);

    if (restricted.restrictedType() ==
        amqp::internal::schema::Restricted::RestrictedTypes::List)
    {
        const auto & list = dynamic_cast<const amqp::internal::schema::List &> (restricted);

        DBG ("Processing List - " << list.listOf() << std::endl); // NOLINT

        if (amqp::internal::schema::Field::typeIsPrimitive(list.listOf())) {
            DBG ("  List of Primitives" << std::endl); // NOLINT
            auto reader = computeIfAbsent<reader::Reader> (
                    m_readersByType,
                    list.listOf(),
                    [& list] () -> std::shared_ptr<reader::PropertyReader> {
                        return reader::PropertyReader::make (list.listOf());
                    });

            return std::make_shared<reader::ListReader> (type_.name(), reader);
        } else {
            DBG ("  List of Composite - " << list.listOf() << std::endl); // NOLINT
            auto reader = m_readersByType[list.listOf()];

            return std::make_shared<reader::ListReader> (list.name(), reader);
        }
    }

    DBG ("  ProcessRestricted: Returning nullptr"); // NOLINT
    return nullptr;
}

/******************************************************************************/

const std::shared_ptr<amqp::internal::reader::IReader>
amqp::internal::
CompositeFactory::byType (const std::string & type_) {
    auto it = m_readersByType.find (type_);

    return (it == m_readersByType.end()) ? nullptr : it->second;
}

/******************************************************************************/

const std::shared_ptr<amqp::internal::reader::IReader>
amqp::internal::
CompositeFactory::byDescriptor (const std::string & descriptor_) {
    auto it = m_readersByDescriptor.find (descriptor_);

    return (it == m_readersByDescriptor.end()) ? nullptr : it->second;
}

/******************************************************************************/

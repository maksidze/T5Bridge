#include <format>

#include <components/esm4/reader.hpp>
#include <components/testing/util.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    void appendInteger(std::string& data, std::uint32_t value, unsigned bytes = 4)
    {
        for (unsigned i = 0; i < bytes; ++i)
            data.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }

    void appendSubrecord(std::string& data, std::string_view type, const std::string& payload)
    {
        data.append(type);
        appendInteger(data, static_cast<std::uint32_t>(payload.size()), 2);
        data.append(payload);
    }

    void appendRecord(std::string& data, std::string_view type, const std::string& payload, std::uint32_t flags = 0)
    {
        data.append(type);
        appendInteger(data, static_cast<std::uint32_t>(payload.size()));
        appendInteger(data, flags);
        appendInteger(data, 0); // Form ID
        appendInteger(data, 0); // Version control
        appendInteger(data, 44, 2); // Skyrim SE form version
        appendInteger(data, 0, 2);
        data.append(payload);
    }

    Files::IStreamPtr makePlugin(const std::vector<std::uint32_t>& ids)
    {
        std::string header;
        appendInteger(header, ESM::VER_170);
        appendInteger(header, 1); // One record follows the header
        appendInteger(header, 0x800);
        std::string subrecords;
        appendSubrecord(subrecords, "HEDR", header);
        std::string plugin;
        appendRecord(plugin, "TES4", subrecords, ESM4::Rec_ESM | ESM4::Rec_Localized);

        subrecords.clear();
        for (const std::uint32_t id : ids)
        {
            std::string value;
            appendInteger(value, id);
            appendSubrecord(subrecords, "FULL", value);
        }
        appendRecord(plugin, "STAT", subrecords);
        return std::make_unique<std::istringstream>(plugin);
    }

    struct StringFormat
    {
        std::string_view mSuffix;
        bool mLengthPrefixed;
    };

    class Esm4LocalizedStrings : public testing::TestWithParam<StringFormat>
    {
    protected:
        std::uint32_t appendString(std::string& data, std::string_view value) const
        {
            const auto offset = static_cast<std::uint32_t>(data.size());
            if (GetParam().mLengthPrefixed)
                appendInteger(data, static_cast<std::uint32_t>(value.size() + 1));
            data.append(value);
            data.push_back('\0');
            return offset;
        }

        void checkStrings(const std::string& data, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& entries,
            const std::vector<std::string>& expected) const
        {
            std::string table;
            appendInteger(table, static_cast<std::uint32_t>(entries.size()));
            appendInteger(table, static_cast<std::uint32_t>(data.size()));
            std::vector<std::uint32_t> ids;
            for (const auto& [id, offset] : entries)
            {
                appendInteger(table, id);
                appendInteger(table, offset);
                ids.push_back(id);
            }
            table.append(data);

            TestingOpenMW::VFSTestFile file(table);
            VFS::FileMap files;
            files.emplace(VFS::Path::Normalized("strings/test_english" + std::string(GetParam().mSuffix)), &file);
            const auto vfs = TestingOpenMW::createTestVFS(std::move(files));
            ESM4::Reader reader(makePlugin(ids), "test.esm", vfs.get(), nullptr);
            ASSERT_TRUE(reader.getRecordHeader());
            reader.getRecordData();
            ASSERT_EQ(ids.size(), expected.size());
            for (std::size_t i = 0; i < ids.size(); ++i)
            {
                ASSERT_TRUE(reader.getSubRecordHeader());
                std::string value = "not read";
                reader.getLocalizedString(value);
                EXPECT_EQ(value, expected[i]) << "String ID " << ids[i];
            }
            EXPECT_FALSE(reader.getSubRecordHeader());
        }
    };

    TEST_P(Esm4LocalizedStrings, readsFirstStringAtOffsetZero)
    {
        std::string data;
        const auto first = appendString(data, "Riverwood");
        const auto second = appendString(data, "Whiterun");
        checkStrings(data, { { 1, first }, { 2, second } }, { "Riverwood", "Whiterun" });
    }

    TEST_P(Esm4LocalizedStrings, sharesStringsAtZeroAndNonzeroOffsetsWithUnsortedEntries)
    {
        std::string data;
        const auto first = appendString(data, "Riverwood");
        const auto second = appendString(data, "Whiterun");
        checkStrings(data, { { 4, second }, { 2, first }, { 3, second }, { 1, first } },
            { "Whiterun", "Riverwood", "Whiterun", "Riverwood" });
    }

    TEST_P(Esm4LocalizedStrings, preservesAnEmptyFirstString)
    {
        std::string data;
        const auto first = appendString(data, "");
        const auto second = appendString(data, "Whiterun");
        checkStrings(data, { { 1, first }, { 2, first }, { 3, second } }, { "", "", "Whiterun" });
    }

    INSTANTIATE_TEST_SUITE_P(StringTables, Esm4LocalizedStrings,
        testing::Values(
            StringFormat{ ".strings", false }, StringFormat{ ".ilstrings", true }, StringFormat{ ".dlstrings", true }));

    TEST(Esm4ReaderDiagnostics, distinguishesObservedHeadersFromWholeAndPartialSkipCalls)
    {
        VFS::Manager vfs;
        ESM4::ReaderDiagnostics diagnostics;
        ESM4::Reader reader(makePlugin({ 1, 2, 3 }), "test.esm", &vfs, nullptr, false, &diagnostics);
        ASSERT_TRUE(reader.getRecordHeader());
        reader.getRecordData();
        ASSERT_TRUE(reader.getSubRecordHeader());
        std::uint32_t first;
        reader.get(first);
        EXPECT_EQ(first, 1);
        ASSERT_TRUE(reader.getSubRecordHeader());
        reader.skipSubRecordData();
        ASSERT_TRUE(reader.getSubRecordHeader());
        std::uint16_t part;
        reader.get(part);
        reader.skipSubRecordData(2);
        EXPECT_FALSE(reader.getSubRecordHeader());

        const auto& full = diagnostics.mSubrecords.at({ ESM4::REC_STAT, ESM::fourCC("FULL") });
        EXPECT_EQ(full.mHeadersSeen, 3);
        EXPECT_EQ(full.mWholeSkipCalls, 1);
        EXPECT_EQ(full.mPartialSkipCalls, 1);
        EXPECT_EQ(full.mBytesExplicitlySkipped, 6);
        EXPECT_EQ(diagnostics.mRecordType, ESM4::REC_STAT);
        EXPECT_EQ(diagnostics.mRecordOffset, 42); // TES4 header plus HEDR subrecord
        EXPECT_EQ(diagnostics.mSubrecordType, ESM::fourCC("FULL"));
        EXPECT_EQ(diagnostics.mSubrecords.at({ ESM4::REC_TES4, ESM::fourCC("HEDR") }).mHeadersSeen, 1);
    }

    TEST(Esm4ReaderDiagnostics, recordsMissingLocalizedLookupsInPermissiveMode)
    {
        VFS::Manager vfs;
        ESM4::ReaderDiagnostics diagnostics;
        ESM4::Reader reader(makePlugin({ 123 }), "test.esm", &vfs, nullptr, true, &diagnostics);
        ASSERT_TRUE(reader.getRecordHeader());
        ASSERT_TRUE(reader.getSubRecordHeader());
        std::string value;
        EXPECT_NO_THROW(reader.getLocalizedString(value));
        EXPECT_EQ(diagnostics.mLocalizedLookups, 1);
        EXPECT_EQ(diagnostics.mLocalizedResolved, 0);
        EXPECT_EQ(diagnostics.mLocalizedMissing, 1);
    }

    TEST(Esm4ReaderDiagnostics, recordsMissingLocalizedLookupsBeforeStrictFailure)
    {
        VFS::Manager vfs;
        ESM4::ReaderDiagnostics diagnostics;
        ESM4::Reader reader(makePlugin({ 123 }), "test.esm", &vfs, nullptr, false, &diagnostics);
        ASSERT_TRUE(reader.getRecordHeader());
        ASSERT_TRUE(reader.getSubRecordHeader());
        std::string value;
        EXPECT_THROW(reader.getLocalizedString(value), std::runtime_error);
        EXPECT_EQ(diagnostics.mLocalizedLookups, 1);
        EXPECT_EQ(diagnostics.mLocalizedResolved, 0);
        EXPECT_EQ(diagnostics.mLocalizedMissing, 1);
        EXPECT_EQ(diagnostics.mRecordOffset, 42);
        EXPECT_EQ(diagnostics.mSubrecordType, ESM::fourCC("FULL"));
    }

    TEST(Esm4ReaderDiagnostics, recordsSuccessfulLocalizedLookups)
    {
        std::string table;
        appendInteger(table, 1);
        appendInteger(table, 10);
        appendInteger(table, 123);
        appendInteger(table, 0);
        table.append("Riverwood", 10);
        TestingOpenMW::VFSTestFile strings(table);
        const auto vfs
            = TestingOpenMW::createTestVFS({ { VFS::Path::NormalizedView("strings/test_english.strings"), &strings } });
        ESM4::ReaderDiagnostics diagnostics;
        ESM4::Reader reader(makePlugin({ 123 }), "test.esm", vfs.get(), nullptr, false, &diagnostics);
        ASSERT_TRUE(reader.getRecordHeader());
        ASSERT_TRUE(reader.getSubRecordHeader());
        std::string value;
        reader.getLocalizedString(value);
        EXPECT_EQ(value, "Riverwood");
        EXPECT_EQ(diagnostics.mLocalizedLookups, 1);
        EXPECT_EQ(diagnostics.mLocalizedResolved, 1);
        EXPECT_EQ(diagnostics.mLocalizedMissing, 0);
    }
}

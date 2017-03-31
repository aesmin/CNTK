//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
#include "stdafx.h"
#define __STDC_FORMAT_MACROS
#define _CRT_SECURE_NO_WARNINGS
#define _SCL_SECURE_NO_WARNINGS
#include <inttypes.h>
#include "MLFIndexer.h"
#include "MLFUtils.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    using namespace std;

    const static char ROW_DELIMITER = '\n';

    MLFIndexer::MLFIndexer(FILE* file, bool frameMode, size_t chunkSize, size_t bufferSize) :
        m_bufferSize(bufferSize),
        m_file(file),
        m_fileOffsetStart(0),
        m_fileOffsetEnd(0),
        m_buffer(bufferSize),
        m_done(false),
        m_index(chunkSize, true, frameMode)
    {
        if (!m_file)
            RuntimeError("Input file not open for reading");
    }

    void MLFIndexer::RefillBuffer()
    {
        if (!m_done)
        {
            // Make sure we always have m_bufferSize elements in side the buffer.
            m_buffer.resize(m_bufferSize);

            // Copy last partial line if needed.
            memcpy(&m_buffer[0], m_lastPartialLineInBuffer.data(), m_lastPartialLineInBuffer.size());
            size_t bytesRead = fread(&m_buffer[0] + m_lastPartialLineInBuffer.size(), 1, m_buffer.size() - m_lastPartialLineInBuffer.size(), m_file);
            if (bytesRead == (size_t)-1)
                RuntimeError("Could not read from the input file.");

            if (bytesRead == 0) // End of file reached.
            {
                m_buffer.clear();
                m_lastPartialLineInBuffer.clear();
                m_fileOffsetStart = m_fileOffsetEnd;
                m_done = true;
                return;
            }

            int lastLF = 0;
            {
                // Let's find the latest \n if exists.
                for (lastLF = (int)m_lastPartialLineInBuffer.size() + (int)bytesRead - 1; lastLF >= 0; lastLF--)
                {
                    if (m_buffer[lastLF] == '\n')
                        break;
                }

                if (lastLF < 0)
                    RuntimeError("Length of MLF sequence cannot exceed '%" PRIu64 "' bytes.", m_bufferSize);
            }

            // Lets cut the buffer at the last end of string, and save partial string 
            // in m_lastLineInBuffer
            auto logicalBufferEnd = lastLF + 1;

            m_fileOffsetStart = m_fileOffsetEnd;
            m_fileOffsetEnd = m_fileOffsetStart + logicalBufferEnd;

            auto lastParialLineSize = m_buffer.size() - logicalBufferEnd;

            // Remember the last possibly parital line.
            m_lastPartialLineInBuffer.resize(lastParialLineSize);
            memcpy(&m_lastPartialLineInBuffer[0], m_buffer.data() + logicalBufferEnd, lastParialLineSize);

            m_buffer.resize(logicalBufferEnd);
        }
    }

    // Building an index of the MLF file:
    //     MLF file -> MLF Header [MLF Utterance]+
    //     MLF Utterance -> Key EOL [Frame Range EOL]+ "." EOL
    // MLF file should start with the MLF header (State::Header -> State:UtteranceKey).
    // Each utterance starts with an utterance key (State::UtteranceKey -> State::UtteranceFrames).
    // End of utterance is indicated by a single dot on a line (State::UtteranceFrames -> State::UtteranceKey)
    void MLFIndexer::Build(CorpusDescriptorPtr corpus)
    {
        if (!m_index.IsEmpty())
            return;

        m_index.Reserve(filesize(m_file));

        RefillBuffer(); // read the first block of data
        if (m_done)
            RuntimeError("Input file is empty");

        size_t id = 0;
        SequenceDescriptor sd = {};
        State currentState = State::Header;
        vector<boost::iterator_range<char*>> lines;
        vector<boost::iterator_range<char*>> tokens;
        bool isValid = true;              // Flag indicating whether the current sequence is valid.
        size_t lastNonEmptyString = 0;    // Needed to parse information about last frame
        size_t sequenceStartOffset = 0;   // Offset in file where current sequence starts.
        while (!m_done)
        {
            lines.clear();
            ReadLines(m_buffer, lines);

            lastNonEmptyString = SIZE_MAX;
            for (size_t i = 0; i < lines.size(); i++)
            {
                if (lines[i].begin() == lines[i].end()) // Skip all empty lines.
                    continue;

                switch (currentState)
                {
                case State::Header:
                {
                    if (string(lines[i].begin(), lines[i].end()) != "#!MLF!#")
                        RuntimeError("Expected MLF header was not found.");
                    currentState = State::UtteranceKey;
                }
                break;
                case State::UtteranceKey:
                {
                    // When several files are appended to a big mlf, there can be
                    // an MLF header between the utterances.
                    if (string(lines[i].begin(), lines[i].end()) == "#!MLF!#")
                        continue;

                    sequenceStartOffset = m_fileOffsetStart + lines[i].begin() - m_buffer.data();
                    isValid = TryParseSequenceKey(lines[i], id, corpus->KeyToId);
                    sd = {};
                    sd.m_key.m_sequence = id;
                    currentState = State::UtteranceFrames;
                }
                break;

                case State::UtteranceFrames:
                {
                    if (distance(lines[i].begin(), lines[i].end()) != 1 || *lines[i].begin() != '.')
                        continue; // Still current utterance.

                    // Ok, a single . on a line means we found the end of the utterance.
                    auto sequenceEndOffset = m_fileOffsetStart + lines[i].end() - m_buffer.data();

                    // Let's find last non empty string and parse information about frames out of it.
                    // Here we assume that the sequence is correct, if not - it will be invalidated later
                    // when the actual data is read.
                    if (lastNonEmptyString != SIZE_MAX)
                        m_lastNonEmptyLine = string(lines[lastNonEmptyString].begin(), lines[lastNonEmptyString].end());

                    if (m_lastNonEmptyLine.empty())
                        isValid = false;
                    else
                    {
                        tokens.clear();
                        auto container = boost::make_iterator_range(&m_lastNonEmptyLine[0], &m_lastNonEmptyLine[0] + m_lastNonEmptyLine.size());
                        boost::split(tokens, container, boost::is_any_of(" "));
                        auto range = MLFFrameRange::ParseFrameRange(tokens);
                        sd.m_numberOfSamples = static_cast<uint32_t>(range.second);
                    }

                    if (isValid)
                        m_index.AddSequence(sd, sequenceStartOffset, sequenceEndOffset);
                    else
                        fprintf(stderr, "WARNING: Cannot parse the utterance '%s' at offset (%" PRIu64 ")\n", corpus->IdToKey(sd.m_key.m_sequence).c_str(), sequenceStartOffset);
                    currentState = State::UtteranceKey; // Let's try the next one.
                }
                break;
                default:
                    LogicError("Unexpected MLF state.");
                }

                lastNonEmptyString = i;
            }

            // Remembering last non empty string to be able to retrieve time frame information 
            // when the dot is just at the beginning of the next buffer.
            if (lastNonEmptyString != SIZE_MAX)
                m_lastNonEmptyLine = string(lines[lastNonEmptyString].begin(), lines[lastNonEmptyString].end());
            else
                m_lastNonEmptyLine.clear();

            RefillBuffer();
        }
    }

    void MLFIndexer::ReadLines(vector<char>& buffer, vector<boost::iterator_range<char*>>& lines)
    {
        lines.clear();
        auto range = boost::make_iterator_range(buffer.data(), buffer.data() + buffer.size());
        boost::split(lines, range, boost::is_any_of("\r\n"));
    }

    // Tries to parse sequence key
    // In MLF a sequence key should be in quotes. During parsing the extension should be removed.
    bool MLFIndexer::TryParseSequenceKey(const boost::iterator_range<char*>& line, size_t& id, function<size_t(const string&)> keyToId)
    {
        id = 0;

        string key(line.begin(), line.end());
        boost::trim_right(key);

        if (key.size() <= 2 || key.front() != '"' || key.back() != '"')
            return false;

        key = key.substr(1, key.size() - 2);
        if (key.size() > 2 && key[0] == '*' && key[1] == '/') // Preserving the old behavior
            key = key.substr(2);

        // Remove extension if specified.
        key = key.substr(0, key.find_last_of("."));

        id = keyToId(key);
        return true;
    }
}}}

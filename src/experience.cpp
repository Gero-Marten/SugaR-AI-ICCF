/*
  SugaR, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2021 The Stockfish developers (see AUTHORS file)

  SugaR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SugaR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <iostream>
#include <iomanip>
#include <string>
#include <string.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <stdio.h> //For: remove()
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include "misc.h"
#include "uci.h"
#include "position.h"
#include "thread.h"
#include "experience.h"

using namespace std;
using namespace Stockfish;

namespace Experience
{
    typedef unordered_map<Key, ExpEntryEx*> ExpMap;
    typedef unordered_map<Key, ExpEntryEx*>::iterator ExpIterator;
    typedef unordered_map<Key, ExpEntryEx*>::const_iterator ExpConstIterator;

    namespace
    {
        const char *ExperienceSignature = "SugaR";
        const size_t ExperienceSignatureLength = strlen(ExperienceSignature) * sizeof(char);

#ifndef NDEBUG
        constexpr size_t WriteBufferSize = 1024;
#else
        constexpr size_t WriteBufferSize = 1024 * 1024 * 16;
#endif
        
        class ExperienceData
        {
        private:
            string                  _filename;
            vector<ExpEntryEx*>     _expExData;

            ExpMap                  _mainExp;
            vector<ExpEntry*>       _newPvExp;
            vector<ExpEntry*>       _newMultiPvExp;

            bool                    _loading;
            atomic<bool>            _abortLoading;      //Only used when destructing
            atomic<bool>            _loadingResult;
            thread                 *_loaderThread;
            condition_variable      _loadingCond;
            mutex                   _loaderMutex;

        private:
            void clear()
            {
                //Make sure we are not loading an experience file
                _abortLoading.store(true, memory_order_relaxed);
                wait_for_load_finished();
                assert(_loaderThread == nullptr);

                //Free
                for (ExpEntryEx *&p : _expExData)
                    free(p);

                //Clear
                _mainExp.clear();
                _expExData.clear();

                //Clear new exp
                clear_new_exp();
            }

            void clear_new_exp()
            {
                //Delete PV experience
                for (const ExpEntry* exp : _newPvExp)
                    delete exp;

                //Delete NonPV experience
                for (const ExpEntry* exp : _newMultiPvExp)
                    delete exp;
                
                //Clear vectors
                _newPvExp.clear();
                _newMultiPvExp.clear();
            }

            bool link_entry(ExpEntryEx *expEx)
            {
                ExpIterator itr = _mainExp.find(expEx->key);

                //If new entry: insert into map and continue
                if (itr == _mainExp.end())
                {
                    _mainExp[expEx->key] = expEx;
                    return true;
                }

                //If existing entry and same move exists then merge
                ExpEntryEx* expEx2 = itr->second->find(expEx->move);
                if (expEx2)
                {
                    expEx2->merge(expEx);
                    return false;
                }

                //If existing entry and different move then insert sorted based on depth/value
                expEx2 = itr->second;
                do
                {
                    if (expEx->compare(expEx2) > 0)
                    {
                        if (expEx2 == itr->second)
                        {
                            itr->second = expEx;
                            expEx->next = expEx2;
                        }
                        else
                        {
                            expEx->next = expEx2->next;
                            expEx2->next = expEx;
                        }

                        return true;
                    }

                    if (!expEx2->next)
                    {
                        expEx2->next = expEx;
                        return true;
                    }

                    expEx2 = expEx2->next;
                } while (true);

                //Should never reach here!
                assert(false);
                return true;
            }

            bool _load(string fn)
            {
                ifstream in(Utility::map_path(fn), ios::in | ios::binary | ios::ate);
                if (!in.is_open())
                {
                    sync_cout << "info string Could not open experience file: " << fn << sync_endl;
                    return false;
                }

                size_t inSize = in.tellg();
                if (inSize == 0)
                {
                    sync_cout << "info string The experience file [" << fn << "] is empty" << sync_endl;
                    return false;
                }

                size_t expDataSize = inSize - ExperienceSignatureLength;
                size_t expCount = expDataSize / sizeof(ExpEntry);
                if (expCount * sizeof(ExpEntry) != expDataSize)
                {
                    sync_cout << "info string Experience file [" << fn << "] is corrupted. Size: " << inSize << ", exp-size: " << expDataSize << ", exp-count: " << expCount << sync_endl;
                    return false;
                }

                //Seek to beginning of file
                in.seekg(ios::beg);

                //Check signature
                char* sig = (char*)malloc(ExperienceSignatureLength);
                if (!sig)
                {
                    sync_cout << "info string Failed to allocate " << ExperienceSignatureLength << " bytes for experience signature verification" << sync_endl;
                    return false;
                }

                if (!in.read(sig, ExperienceSignatureLength))
                {
                    sync_cout << "info string Failed to read " << ExperienceSignatureLength << " bytes for experience signature verification" << sync_endl;
                    return false;
                }

                if (memcmp(sig, ExperienceSignature, ExperienceSignatureLength) != 0)
                {
                    free(sig);

                    sync_cout << "info string Experience file [" << fn << "] signature missmatch " << sync_endl;
                    return false;
                }

                //Free signature memory
                free(sig);

                //Allocate buffer for ExpEx data
                ExpEntryEx* expData = (ExpEntryEx*)malloc(expCount * sizeof(ExpEntryEx));
                if (!expData)
                {
                    sync_cout << "info string Failed to allocate " << expCount * sizeof(ExpEntryEx) << " bytes for stored experience data from file [" << fn << "]" << sync_endl;
                    return false;
                }

                //Few variables to be used for statistical information
                size_t prevPosCount = _mainExp.size();

                //Load experience entries
                size_t duplicateMoves = 0;
                for (size_t i = 0; i < expCount; i++)
                {
                    if (_abortLoading.load(memory_order_relaxed))
                        break;

                    //Prepare to read
                    ExpEntryEx* expEx = expData + i;
                    expEx->next = nullptr;

                    //Read
                    if (!in.read((char*)expEx, sizeof(ExpEntry)))
                    {
                        free(expData);

                        sync_cout << "info string Failed to read " << sizeof(ExpEntryEx) << " bytes of experience entry " << i + 1 << " of " << expCount << sync_endl;
                        return false;
                    }

                    //Merge
                    if (!link_entry(expEx))
                        duplicateMoves++;
                }

                //Add buffer to vector so that it will be released later
                _expExData.push_back(expData);

                //Nothing to do if loading was aborted
                if (_abortLoading.load(memory_order_relaxed))
                    return false;

                //Show some statistics
                if (prevPosCount)
                {
                    sync_cout
                        << "info string " << fn << " -> Total new moves: " << expCount
                        << ". Total new positions: " << (_mainExp.size() - prevPosCount)
                        << ". Duplicate moves: " << duplicateMoves
                        << sync_endl;
                }
                else
                {
                    sync_cout
                        << "info string " << fn << " -> Total moves: " << expCount
                        << ". Total positions: " << _mainExp.size()
                        << ". Duplicate moves: " << duplicateMoves
                        << ". Fragmentation: " << setprecision(2) << fixed << 100.0 * (double)duplicateMoves / (double)expCount << "%"
                        << sync_endl;
                }

                return true;
            }

            bool _save(string fn, bool saveAll)
            {
                fstream out;
                out.open(Utility::map_path(fn), ios::out | ios::binary | ios::app);
                if (!out.is_open())
                {
                    sync_cout << "info string Failed to open experience file [" << fn << "] for writing" << sync_endl;
                    return false;
                }

                //If this is a new file then we need to write the signature first
                out.seekg(0, out.end);
                size_t length = out.tellg();
                out.seekg(0, out.beg);

                if (length == 0)
                {
                    out.seekp(0, out.beg);

                    if (!out.write(ExperienceSignature, ExperienceSignatureLength))
                    {
                        sync_cout << "info string Failed to write signature to experience file [" << fn << "]" << sync_endl;
                        return false;
                    }
                }

                //Reposition writing pointer to end of file
                out.seekp(ios::end);

                vector<char> writeBuffer;
                writeBuffer.reserve(WriteBufferSize);

                auto write_entry = [&](const ExpEntry* exp, bool force) -> bool
                {
                    if (exp)
                    {
                        const char* data = reinterpret_cast<const char*>(exp);
                        writeBuffer.insert(writeBuffer.end(), data, data + sizeof(ExpEntry));
                    }

                    bool success = true;
                    if (force || writeBuffer.size() >= WriteBufferSize)
                    {
                        out.write(writeBuffer.data(), writeBuffer.size());
                        if (!out)
                            success = false;

                        writeBuffer.clear();
                    }

                    return success;
                };

                size_t allMoves = 0;
                size_t allPositions = 0;
                if (saveAll)
                {
                    const ExpEntryEx* p = nullptr;
                    for (const auto& x : _mainExp)
                    {
                        allPositions++;
                        p = x.second;

                        while (p)
                        {
                            if (p->depth >= MIN_EXP_DEPTH)
                            {
                                allMoves++;
                                if (!write_entry(p, false))
                                {
                                    sync_cout << "info string Failed to save experience entry to experience file [" << fn << "]" << sync_endl;
                                    return false;
                                }
                            }

                            p = p->next;
                        }
                    }
                }

                //Save new PV experience
                int newPvExpCount = 0;
                for (const ExpEntry* e : _newPvExp)
                {
                    if (!e)
                        continue;

                    if (e->depth < MIN_EXP_DEPTH)
                        continue;

                    if (!write_entry(e, false))
                    {
                        sync_cout << "info string Failed to save new PV experience entry to experience file [" << fn << "]" << sync_endl;
                        return false;
                    }

                    newPvExpCount++;
                }

                //Save new MultiPV experience
                int newMultiPvExpCount = 0;
                for (const ExpEntry* e : _newMultiPvExp)
                {
                    if (!e)
                        continue;

                    if (e->depth < MIN_EXP_DEPTH)
                        continue;

                    if (!write_entry(e, false))
                    {
                        sync_cout << "info string Failed to save new MultiPV experience entry to experience file [" << fn << "]" << sync_endl;
                        return false;
                    }

                    newMultiPvExpCount++;
                }

                //Flush buffer
                write_entry(nullptr, true);

                //Clear new moves
                clear_new_exp();

                if (saveAll)
                {
                    sync_cout << "info string Saved " << allPositions << " position(s) and " << allMoves << " moves to experience file: " << fn << sync_endl;
                }
                else
                {
                    sync_cout << "info string Saved " << newPvExpCount << " PV and " << newMultiPvExpCount << " MultiPV entries to experience file: " << fn << sync_endl;
                }

                return true;
            }

        public:
            ExperienceData()
            {
                _loading = false;
                _abortLoading.store(false, memory_order_relaxed);
                _loadingResult.store(false, memory_order_relaxed);
                _loaderThread = nullptr;
            }

            ~ExperienceData()
            {
                clear();
            }

            string filename() const
            {
                return _filename;
            }

            bool has_new_exp() const
            {
                return _newPvExp.size() || _newMultiPvExp.size();
            }

            bool load(string filename, bool synchronous)
            {
                //Make sure we are not already in the process of loading same/other experience file
                wait_for_load_finished();

                //Load requested experience file
                _filename = filename;
                _loadingResult.store(false, memory_order_relaxed);

                //Block
                {
                    _loading = true;
                    lock_guard<mutex> lg1(_loaderMutex);
                    _loaderThread = new thread(thread([this, filename]()
                        {
                            //Load
                            bool loadingResult = _load(filename);
                            _loadingResult.store(loadingResult, memory_order_relaxed);

                            //Notify
                            {
                                lock_guard<mutex> lg2(_loaderMutex);
                                _loading = false;
                                _loadingCond.notify_one();
                            }
                            
                            //Detach and delete thread
                            _loaderThread->detach();
                            delete _loaderThread;
                            _loaderThread = nullptr;
                        }));
                }

                return synchronous ? wait_for_load_finished() : true;
            }

            bool wait_for_load_finished()
            {
                unique_lock<mutex> ul(_loaderMutex);
                _loadingCond.wait(ul, [&] { return !_loading; });
                return loading_result();
            }

            bool loading_result() const
            {
                return _loadingResult.load(memory_order_relaxed);
            }

            void save(string fn, bool saveAll)
            {
                //Make sure we are not already in the process of loading same/other experience file
                wait_for_load_finished();

                if (!has_new_exp() && (!saveAll || _mainExp.size() == 0))
                    return;

                //Step 1: Create backup only if 'saveAll' is 'true'
                string expFilename = Utility::map_path(fn);
                string backupExpFilename;
                if (saveAll && Utility::file_exists(expFilename))
                {
                    backupExpFilename = expFilename + ".bak";

                    //If backup file already exists then delete it
                    if (Utility::file_exists(backupExpFilename))
                    {
                        if (remove(backupExpFilename.c_str()) != 0)
                        {
                            sync_cout << "info string Could not deleted existing backup file: " << backupExpFilename << sync_endl;

                            //Clear backup filename
                            backupExpFilename.clear();
                        }
                    }

                    //Rename current experience file
                    if (!backupExpFilename.empty())
                    {
                        if (rename(expFilename.c_str(), backupExpFilename.c_str()) != 0)
                        {
                            sync_cout << "info string Could not create backup of current experience file" << sync_endl;

                            //Clear backup filename
                            backupExpFilename.clear();
                        }
                    }
                }

                //Step 2: Save
                if (!_save(fn, saveAll))
                {
                    //Step 2a: Restore backup in case of failure while saving
                    if (!backupExpFilename.empty())
                    {
                        if (rename(backupExpFilename.c_str(), expFilename.c_str()) != 0)
                        {
                            sync_cout << "info string Could not restore backup experience file: " << backupExpFilename << sync_endl;
                        }
                    }
                }
            }

            const ExpEntryEx* probe(Key k)
            {
                ExpConstIterator itr = _mainExp.find(k);
                if (itr == _mainExp.end())
                    return nullptr;

                assert(itr->second->key == k);

                return itr->second;
            }

            void add_pv_experience(Key k, Move m, Value v, Depth d)
            {
                _newPvExp.emplace_back(new ExpEntry(k, m, v, d));
            }

            void add_multipv_experience(Key k, Move m, Value v, Depth d)
            {
                _newMultiPvExp.emplace_back(new ExpEntry(k, m, v, d));
            }
        };

        ExperienceData*currentExperience = nullptr;
        bool experienceEnabled = true;
        bool learningPaused = false;
    }

    void init()
    {
        experienceEnabled = Options["Experience Enabled"];
        if (!experienceEnabled)
        {
            unload();
            return;
        }

        string filename = Options["Experience File"];
        if (currentExperience)
        {
            if (currentExperience->filename() == filename && currentExperience->loading_result())
                return;

            if (currentExperience)
                unload();
        }

        currentExperience = new ExperienceData();
        currentExperience->load(filename, false);
    }

    bool enabled()
    {
        return experienceEnabled;
    }

    void unload()
    {
        save();

        delete currentExperience;
        currentExperience = nullptr;
    }

    void save()
    {
        if (!currentExperience || !currentExperience->has_new_exp() || (bool)Options["Experience Readonly"])
            return;

        currentExperience->save(currentExperience->filename(), false);
    }

    void reload()
    {
        if (!currentExperience || !currentExperience->has_new_exp())
            return;

        init();
    }

    const ExpEntryEx* probe(Key k)
    {
        assert(experienceEnabled);
        if (!currentExperience)
            return nullptr;

        return currentExperience->probe(k);
    }

    void wait_for_loading_finished()
    {
        if (!currentExperience)
            return;

        currentExperience->wait_for_load_finished();
    }

    //Defrag command:
    //Format:  defrag [filename]
    //Example: defrag C:\Path to\Experience\file.exp
    //Note:    'filename' is optional. If omitted, then the default experience filename (SugaR.exp) will be used
    //         'filename' can contain spaces and can be a full path. If filename contains spaces, it is best to enclose it in quotations
    void defrag(int argc, char* argv[])
    {
        //Make sure experience has finished loading
        //Not exactly needed here, but the messages shown when exp loading finish will
        //disturb the progress messages shown by this function
        wait_for_loading_finished();

        if (argc != 1)
        {
            sync_cout << "info string Error : Incorrect defrag command" << sync_endl;
            sync_cout << "info string Syntax: defrag [filename]" << sync_endl;
            return;
        }

        string filename = Utility::map_path(Utility::unquote(argv[0]));

        //Print message
        sync_cout << "\nDefragmenting experience file: " << filename << sync_endl;

        //Map filename
        filename = Utility::map_path(filename);

        //Load
        ExperienceData exp;
        if (!exp.load(filename, true))
            return;

        //Save
        exp.save(filename, true);
    }

    //Merge command:
    //Format:  merge filename filename1 filename2 ... filenameX
    //Example: defrag "C:\Path to\Experience\file.exp"
    //Note:    'filename' is the target filename, which will also merged with the rest of the files if it exists
    //         'filename1' ... 'filenameX' are the names of the experience files to be merged (along with filename)
    //         'filename' can contain spaces but in that case it needs to eb quoted. It can also be a full path
    void merge(int argc, char* argv[])
    {
        //Make sure experience has finished loading
        //Not exactly needed here, but the messages shown when exp loading finish will
        //disturb the progress messages shown by this function
        wait_for_loading_finished();

        //Step 1: Check
        if (argc < 2)
        {
            sync_cout << "info string Error : Incorrect merge command" << sync_endl;
            sync_cout << "info string Syntax: merge <filename> <filename1> [filename2] ... [filenameX]" << sync_endl;
            sync_cout << "info string The first <filename> is also the target experience file which will contain all the merged data" << sync_endl;
            sync_cout << "info string The files <filename1> ... <filenameX> are the other experience files to be merged" << sync_endl;
            return;
        }

        //Step 2: Collect filenames
        vector<string> filenames;
        for (int i = 0; i < argc; ++i)
            filenames.push_back(Utility::map_path(Utility::unquote(argv[i])));

        //Step 3: The first filename is also the target filename
        string targetFilename = filenames.front();

        //Print message
        sync_cout << "\nMerging experience files: ";
        for (const string& fn : filenames)
            cout << "\n\t" << fn;

        cout << "\nTarget file: " << targetFilename << "\n" << sync_endl;

        //Step 4: Load and merge
        ExperienceData exp;
        for (const string& fn : filenames)
            exp.load(fn, true);

        exp.save(targetFilename, true);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //Convert compact PGN data to experience entries
    //
    //Compact PGN consists of the following format:
    // {fen-string,w|b|d,move[:score:depth],move[:score:depth],move:score:depth[:score:depth],...}
    //
    // *) fen-string: Represents the start position of the game, which is not necesserly the normal start position
    // *) w|b|d: Indicates the game result from PGN (to be validated), w= white win, b = black win, d = draw
    // *) move[:score:depth]
    //      - move : The move in long algebraic form, example e2e4
    //      - score: The engine evaluation of the position from side to move point of view. This is an optional field
    //      - depth: The depth of the move as read from engine evaluation. This is an optional field
    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
    void convert_compact_pgn(int argc, char* argv[])
    {
        //Make sure experience has finished loading
        //Not exactly needed here, but the messages shown when exp loading finish will
        //disturb the progress messages shown by this function
        wait_for_loading_finished();

        if (argc < 2)
        {
            sync_cout << "Expecting at least 2 arguments, received: " << argc << sync_endl;
            return;
        }

        //////////////////////////////////////////////////////////////////////////
        // Collect input
        string inputPath  = Utility::unquote(argv[0]);
        string outputPath = Utility::unquote(argv[1]);
        int maxPly     = argc >= 3 ? atoi(argv[2])        : 1000;
        Value maxValue = argc >= 4 ? (Value)atoi(argv[3]) : (Value)VALUE_MATE;
        Depth minDepth = argc >= 5 ? std::max((Depth)atoi(argv[4]), MIN_EXP_DEPTH) : MIN_EXP_DEPTH;
        Depth maxDepth = argc >= 6 ? std::max((Depth)atoi(argv[5]), MIN_EXP_DEPTH) : (Depth)MAX_PLY;

        sync_cout                                         << endl
                  << "Building experience from PGN: "     << endl
                  << "\tCompact PGN file: " << inputPath  << endl
                  << "\tExperience file : " << outputPath << endl
                  << "\tMax ply         : " << maxPly     << endl
                  << "\tMax value       : " << maxValue   << endl
                  << "\tDepth range     : " << minDepth   << " - " << maxDepth
                                                          << endl << sync_endl;

        //////////////////////////////////////////////////////////////////
        //Conversion information
        struct GLOBAL_COMPACT_PGN_CONVERSION_DATA
        {
            //Game statistics
            size_t numGames = 0;
            size_t numGamesWithErrors = 0;
            size_t numGamesIgnored = 0;

            //Move statistics
            size_t numMovesWithScores = 0;
            size_t numMovesWithScoresIgnored = 0;
            size_t numMovesWithoutScores = 0;                        

            //WBD statistics
            size_t wbd[COLOR_NB + 1] = { 0, 0, 0 };

            //Input stream
            fstream inputStream;
            size_t inputStreamSize = 0;

            //Output stream
            fstream outputStream;
            size_t outputStreamBase;

            //Buffer
            vector<char> buffer;
        }globalConversionData;

        //////////////////////////////////////////////////////////////////
        //Game conversion information
        struct COMPACT_PGN_CONVERSION_DATA
        {
            Color detectedWinnerColor;
            bool drawDetected;
            int resultWeight[COLOR_NB + 1];

            Position pos;

            COMPACT_PGN_CONVERSION_DATA()
            {
                clear();
            }

            void clear()
            {
                detectedWinnerColor = COLOR_NB;
                drawDetected = false;
                memset((void*)&resultWeight, 0, sizeof(resultWeight));
            }
        }gameData;

        //////////////////////////////////////////////////////////////////////////
        //Input stream
        globalConversionData.inputStream.open(inputPath, ios::in | ios::ate);
        if (!globalConversionData.inputStream.is_open())
        {
            sync_cout << "Could not open <" << inputPath << "> for reading" << sync_endl;
            return;
        }

        globalConversionData.inputStreamSize = globalConversionData.inputStream.tellg();
        globalConversionData.inputStream.seekg(0, ios::beg);

        //////////////////////////////////////////////////////////////////////////
        //Output stream
        globalConversionData.outputStream.open(outputPath, ios::out | ios::binary | ios::app | ios::ate);
        if (!globalConversionData.outputStream.is_open())
        {
            sync_cout << "Could not open <" << outputPath << "> for writing" << sync_endl;
            return;
        }

        globalConversionData.outputStreamBase = globalConversionData.outputStream.tellp();

        //If the output file is a new file, then we need to write the signature
        if (globalConversionData.outputStreamBase == 0)
        {
            globalConversionData.outputStream.write(ExperienceSignature, ExperienceSignatureLength);
            globalConversionData.outputStreamBase = globalConversionData.outputStream.tellp();
        }

        //////////////////////////////////////////////////////////////////////////
        //Buffer
        globalConversionData.buffer.reserve(WriteBufferSize);

        //////////////////////////////////////////////////////////////////
        //Experience Data writing routine
        auto write_data = [&](bool force)
        {
            if (force || globalConversionData.buffer.size() >= WriteBufferSize)
            {
                globalConversionData.outputStream.write(globalConversionData.buffer.data(), globalConversionData.buffer.size());
                globalConversionData.buffer.clear();

                size_t numMoves = globalConversionData.numMovesWithScores + globalConversionData.numMovesWithScoresIgnored + globalConversionData.numMovesWithoutScores;
                size_t inputStreamPos = globalConversionData.inputStream.tellg();

                //Fix for end-of-input stream value of -1!
                if (inputStreamPos == (size_t)-1)
                    inputStreamPos = globalConversionData.inputStreamSize;

                sync_cout
                    << fixed << setprecision(2) << setw(6) << setfill(' ') << ((double)inputStreamPos * 100.0 / (double)globalConversionData.inputStreamSize) << "% ->"
                    << " Games: " << globalConversionData.numGames << " (errors: " << globalConversionData.numGamesWithErrors << "),"
                    << " WBD: " << globalConversionData.wbd[WHITE] << "/" << globalConversionData.wbd[BLACK] << "/" << globalConversionData.wbd[COLOR_NB] << ","
                    << " Moves: " << numMoves << " (" << globalConversionData.numMovesWithScores << " with scores, " << globalConversionData.numMovesWithoutScores << " without scores, " << globalConversionData.numMovesWithScoresIgnored << " ignored)."
                    << " Exp size: " << format_bytes((size_t)globalConversionData.outputStream.tellp() - globalConversionData.outputStreamBase, 2)
                    << sync_endl;
            }
        };

        //////////////////////////////////////////////////////////////////
        //Helper function for splitting strings
        auto tokenize = [](const string& str, char delimiter) -> vector<string>
        {
            istringstream iss(str);
            vector<string> fields;

            string field;
            while (getline(iss, field, delimiter))
                fields.push_back(field);

            return fields;
        };

        //////////////////////////////////////////////////////////////////
        //Conversion routine
        auto convert_compact_pgn_to_exp = [&](const string &compactPgn) -> bool
        {
            constexpr Value GOOD_SCORE = PawnValueEg * 3;
            constexpr Value OK_SCORE = GOOD_SCORE / 2;
            constexpr Value MAX_DRAW_SCORE = (Value)50;
            constexpr int MIN_WEIGHT_FOR_DRAW = 8;
            constexpr int MIN_WEIGHT_FOR_WIN = 16;
            constexpr int MIN_PLY_PER_GAME = 16;

            //Clear current game data
            gameData.clear();

            //Increment games counter
            ++globalConversionData.numGames;

            //Split compact PGN into its main three parts
            vector<string> tokens = tokenize(compactPgn, ',');

            if (tokens.size() < 3)
            {
                ++globalConversionData.numGamesWithErrors;
                return false;
            }

            //////////////////////////////////////////////////////////////////
            //Read FEN string
            string fen = tokens[0];

            //Setup Position
            StateListPtr states(new deque<StateInfo>(1));
            gameData.pos.set(fen, false, &states->back(), Threads.main());

            //////////////////////////////////////////////////////////////////
            //Read result
            string resultStr = tokens[1];

            //Find winner color from result-string
            Color winnerColor;
                 if (resultStr == "w") winnerColor = WHITE;
            else if (resultStr == "b") winnerColor = BLACK;
            else if (resultStr == "d") winnerColor = COLOR_NB;
            else                       return false;

            //////////////////////////////////////////////////////////////////
            // Read moves
            int gamePly = 0;
            ExpEntry tempExp((Key)0, MOVE_NONE, VALUE_NONE, DEPTH_NONE);
            vector<char> tempBuffer;
            for (size_t i = 2; i < tokens.size(); ++i)
            {
                ++gamePly;

                //Get move and score
                string _move;
                string _score;
                string _depth;

                vector<string> tok = tokenize(tokens[i], ':');
                if (tok.size() >= 1) _move = tok[0];
                if (tok.size() >= 2) _score = tok[1];
                if (tok.size() >= 3) _depth = tok[2];

                if (tok.size() >= 4)
                {
                    ++globalConversionData.numGamesWithErrors;
                    return false;
                }

                //Cleanup move
                while (_move.back() == '+' || _move.back() == '#' || _move.back() == '\r' || _move.back() == '\n')
                    _move.pop_back();

                //Check if move is empty
                if (_move.empty())
                {
                    ++globalConversionData.numGamesWithErrors;
                    return false;
                }

                //Parse the move
                Move move = UCI::to_move(gameData.pos, _move);
                if (move == MOVE_NONE)
                {
                    ++globalConversionData.numGamesWithErrors;
                    return false;
                }

                Depth depth = _depth.empty() ? DEPTH_NONE : (Depth)stoi(_depth);
                Value score = _score.empty() ? VALUE_NONE : (Value)stoi(_score);

                if (depth != DEPTH_NONE && score != VALUE_NONE)
                {
                    if (depth >= minDepth && depth <= maxDepth && abs(score) <= maxValue)
                    {
                        ++globalConversionData.numMovesWithScores;

                        //Assign to temporary experience
                        tempExp.key = gameData.pos.key();
                        tempExp.move = move;
                        tempExp.value = score;
                        tempExp.depth = depth;

                        //Add to global buffer
                        const char* data = reinterpret_cast<const char*>(&tempExp);
                        tempBuffer.insert(tempBuffer.end(), data, data + sizeof(tempExp));
                    }
                    else
                    {
                        ++globalConversionData.numMovesWithScoresIgnored;
                    }

                    //////////////////////////////////////////////////////////////////
                    //Guess game result and apply sanity checks (we can't trust PGN scores blindly)
                    if (abs(score) >= VALUE_KNOWN_WIN)
                    {
                        Color winnerColorBasedOnThisMove = score > 0 ? gameData.pos.side_to_move() : ~gameData.pos.side_to_move();
                        if (gameData.detectedWinnerColor == COLOR_NB)
                        {
                            gameData.detectedWinnerColor = winnerColorBasedOnThisMove;
                            if (gameData.detectedWinnerColor != winnerColor)
                            {
                                ++globalConversionData.numGamesIgnored;
                                return false;
                            }
                        }
                        else if (gameData.detectedWinnerColor != winnerColorBasedOnThisMove)
                        {
                            ++globalConversionData.numGamesIgnored;
                            return false;
                        }
                    }
                    else if (gameData.pos.is_draw(gameData.pos.is_draw(gameData.pos.game_ply())))
                    {
                        gameData.drawDetected = true;
                    }

                    //Detect score pattern
                    if (abs(score) >= GOOD_SCORE)
                    {
                        gameData.resultWeight[COLOR_NB] = 0;
                        gameData.resultWeight[score > 0 ? gameData.pos.side_to_move() : ~gameData.pos.side_to_move()] += score < 0 ? 4 : 2;
                        gameData.resultWeight[score > 0 ? ~gameData.pos.side_to_move() : gameData.pos.side_to_move()] = 0;
                    }
                    else if (abs(score) >= OK_SCORE)
                    {
                        gameData.resultWeight[COLOR_NB] /= 2;
                        gameData.resultWeight[score > 0 ? gameData.pos.side_to_move() : ~gameData.pos.side_to_move()] += score < 0 ? 2 : 1;
                        gameData.resultWeight[score > 0 ? ~gameData.pos.side_to_move() : gameData.pos.side_to_move()] /= 2;
                    }
                    else if (abs(score) <= MAX_DRAW_SCORE)
                    {
                        gameData.resultWeight[COLOR_NB] += 2;
                        gameData.resultWeight[WHITE] = 0;
                        gameData.resultWeight[BLACK] = 0;
                    }
                    else
                    {
                        gameData.resultWeight[COLOR_NB] += 1;
                        gameData.resultWeight[WHITE] /= 2;
                        gameData.resultWeight[BLACK] /= 2;
                    }
                }
                else
                {
                    ++globalConversionData.numMovesWithoutScores;
                }

                //Do the move
                states->emplace_back();
                gameData.pos.do_move(move, states->back());

                //////////////////////////////////////////////////////////////////
                //Detect draw by insufficient material
                if (!gameData.drawDetected)
                {
                    int num_pieces = gameData.pos.count<ALL_PIECES>();

                    if (num_pieces == 2) //KvK
                    {
                        gameData.drawDetected = true;
                    }
                    else if (num_pieces == 3 && (gameData.pos.count<BISHOP>() + gameData.pos.count<KNIGHT>()) == 1) //KvK + 1 minor piece
                    {
                        gameData.drawDetected = true;
                    }
                    else if (num_pieces == 4 && gameData.pos.count<BISHOP>(WHITE) == 1 && gameData.pos.count<BISHOP>(BLACK) == 1) //KBvKB, bishops of the same color
                    {
                        if (
                            ((gameData.pos.pieces(WHITE, BISHOP) & DarkSquares) && (gameData.pos.pieces(BLACK, BISHOP) & DarkSquares))
                            || ((gameData.pos.pieces(WHITE, BISHOP) & ~DarkSquares) && (gameData.pos.pieces(BLACK, BISHOP) & ~DarkSquares)))
                            gameData.drawDetected = true;
                    }
                }

                //If draw is detected but game result isn't draw then reject the game
                if (gameData.drawDetected && gameData.detectedWinnerColor != COLOR_NB)
                {
                    ++globalConversionData.numGamesIgnored;
                    return false;
                }
            }

            //Does the game have enough moves?
            if (gamePly < MIN_PLY_PER_GAME)
            {
                ++globalConversionData.numGamesIgnored;
                return false;
            }

            //If winner isn't yet identified, check result weights and try to identify it
            if (gameData.detectedWinnerColor == COLOR_NB)
            {
                if (gameData.resultWeight[WHITE] >= MIN_WEIGHT_FOR_WIN)
                    gameData.detectedWinnerColor = WHITE;
                else if (gameData.resultWeight[BLACK] >= MIN_WEIGHT_FOR_WIN)
                    gameData.detectedWinnerColor = BLACK;
            }

            //////////////////////////////////////////////////////////////////
            //More sanity checks
            if (   (gameData.detectedWinnerColor != winnerColor)
                || (winnerColor != COLOR_NB && gameData.resultWeight[winnerColor] < MIN_WEIGHT_FOR_WIN)
                || (winnerColor == COLOR_NB && !gameData.drawDetected && gameData.resultWeight[COLOR_NB] < MIN_WEIGHT_FOR_DRAW))
            {
                ++globalConversionData.numGamesIgnored;
                return false;
            }

            //Update WBD stats
            ++globalConversionData.wbd[winnerColor];

            //Copy to global buffer
            globalConversionData.buffer.insert(globalConversionData.buffer.end(), tempBuffer.begin(), tempBuffer.end());

            return true;
        };

        //////////////////////////////////////////////////////////////////
        //Loop
        string line;
        while (getline(globalConversionData.inputStream, line))
        {
            //Skip empty lines
            if (line.empty())
                continue;

            if (line.front() != '{' || line.back() != '}')
                continue;

            line = line.substr(1, line.size() - 2);

            if (convert_compact_pgn_to_exp(line))
                write_data(false);
        }

        //Final commit
        write_data(true);

        //////////////////////////////////////////////////////////////////
        //Defragment outouf file
        if (globalConversionData.numMovesWithScores)
        {
            //If we don't close the output stream here then defragmentation will not be able to create a backup of the file!
            globalConversionData.outputStream.close();

            sync_cout << "Conversion complete" << endl << endl << "Defragmenting: " << outputPath << sync_endl;

            ExperienceData exp;
            if (!exp.load(outputPath, true))
                return;

            //Save
            exp.save(outputPath, true);
        }
    }

    void show_exp(Position& pos, bool extended)
    {
        //Make sure experience has finished loading
        wait_for_loading_finished();

        sync_cout << pos << endl;

        cout << "Experience: ";
        const ExpEntryEx* expEx = Experience::probe(pos.key());
        if (!expEx)
        {
            cout << "No experience data found for this position" << sync_endl;
            return;
        }

        const int experienceBookMovesAhead = 8;

        //Find expected score for all experience moves by playing the next 'experienceBookMovesAhead' moves
        Color sideToMove = pos.side_to_move();
        vector<pair<const ExpEntryEx*, Value>> estimatedValue;
        const ExpEntryEx* tempExpEx = expEx;        
        while (tempExpEx)
        {
            //Start fresh
            StateInfo st[experienceBookMovesAhead];
            vector<const ExpEntryEx*> exp; //For undoing of exp moves

            //Find the estimated value of the next 'experienceBookMovesAhead' moves
            int multiplier = 1;
            int64_t valueSum = 0;
            int64_t valueWeight = 0;
            const Experience::ExpEntryEx* nextPosExpEx = tempExpEx;
            while (nextPosExpEx && nextPosExpEx->depth >= MIN_EXP_DEPTH && exp.size() < experienceBookMovesAhead)
            {
                //Add this exp
                valueSum += nextPosExpEx->value * nextPosExpEx->depth * multiplier * (pos.side_to_move() == sideToMove ? 1 : -1);

                //Adjust 'multiplierSum' and increase the multiplier
                valueWeight += nextPosExpEx->depth * multiplier;
                multiplier++;

                //Do the exp move and probe the next one
                exp.push_back(nextPosExpEx);
                pos.do_move(nextPosExpEx->move, st[exp.size() - 1]);

                nextPosExpEx = Experience::probe(pos.key());

                //Find best next experience move (shallow search)
                const Experience::ExpEntryEx* t = nextPosExpEx ? nextPosExpEx->next : nullptr;
                while (t)
                {
                    if (t->compare(nextPosExpEx))
                        nextPosExpEx = t;

                    t = t->next;
                }
            }

            if (exp.size())
            {
                //Undo the moves
                for (auto it = exp.rbegin(); it != exp.rend(); ++it)
                    pos.undo_move((*it)->move);

                //Calculate estimated sum
                estimatedValue.emplace_back(tempExpEx, Value(valueSum / valueWeight));
            }
            else
            {
                estimatedValue.emplace_back(tempExpEx, VALUE_NONE);
            }

            //Next
            tempExpEx = tempExpEx->next;
        }

        //Sort experience moves based on estimatedValue
        stable_sort(
            estimatedValue.begin(),
            estimatedValue.end(),
            [](const pair<const ExpEntryEx*, Value> &a, const pair<const ExpEntryEx*, Value> &b)
            {
                if (a.second != VALUE_NONE && b.second != VALUE_NONE)
                    return a.second > b.second;

                if (a.second == VALUE_NONE)
                    return false;

                return true;
            });

        cout << endl;
        int expCount = 0;
        for(const pair<const ExpEntryEx*, Value>& pr : estimatedValue)
        {
            cout
                << setw(2) << setfill(' ') << left << ++expCount << ": "
                << setw(5) << setfill(' ') << left << UCI::move(pr.first->move, pos.is_chess960())
                << ", depth: " << setw(2) << setfill(' ') << left << pr.first->depth
                << ", eval: " << setw(6) << setfill(' ') << left << UCI::value(pr.first->value, pr.first->value);

            if (extended)
            {
                if(pr.second != VALUE_NONE)
                    cout << ", quality: " << setw(6) << setfill(' ') << left << pr.second;
                else
                    cout << ", quality: " << setw(6) << setfill(' ') << left << "N/A";
            }

            cout << endl;

            expEx = expEx->next;
        }

        cout << sync_endl;
    }

    void pause_learning()
    {
        learningPaused = true;
    }

    void resume_learning()
    {
        learningPaused = false;
    }

    bool is_learning_paused()
    {
        return learningPaused;
    }

    void add_pv_experience(Key k, Move m, Value v, Depth d)
    {
        if (!currentExperience)
            return;

        assert((bool)Options["Experience Readonly"] == false);

        currentExperience->add_pv_experience(k, m, v, d);
    }

    void add_multipv_experience(Key k, Move m, Value v, Depth d)
    {
        if (!currentExperience)
            return;

        assert((bool)Options["Experience Readonly"] == false);

        currentExperience->add_multipv_experience(k, m, v, d);
    }
}


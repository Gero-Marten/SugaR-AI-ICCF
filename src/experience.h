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

#ifndef __LEARN_H__
#define __LEARN_H__

#include "types.h"

using namespace std;

#define MIN_EXP_DEPTH       ((Depth)4)

namespace Experience
{
    struct ExpEntry
    {
        Stockfish::Key     key;        //8 bytes
        Stockfish::Move    move;       //4 bytes
        Stockfish::Value   value;      //4 bytes
        Stockfish::Depth   depth;      //4 bytes
        uint8_t padding[4];            //4 bytes

        ExpEntry() = delete;
        ExpEntry(const ExpEntry& exp) = delete;
        ExpEntry& operator =(const ExpEntry& exp) = delete;
        
        explicit ExpEntry(Stockfish::Key k, Stockfish::Move m, Stockfish::Value v, Stockfish::Depth d);
        void merge(const ExpEntry* exp);
        int compare(const ExpEntry* exp) const;
    };

    static_assert(sizeof(ExpEntry) == 24);

    //Experience structure
    struct ExpEntryEx : public ExpEntry
    {
        ExpEntryEx* next = nullptr;

        ExpEntryEx() = delete;
        ExpEntryEx(const ExpEntryEx& expEx) = delete;
        ExpEntryEx &operator =(const ExpEntryEx& expEx) = delete;

        ExpEntryEx* find(Stockfish::Move m);
        std::pair<Stockfish::Value, bool> quality(Stockfish::Position &pos) const;
    };

    void init();
    bool enabled();

    void unload();
    void save();
    void reload();

    void wait_for_loading_finished();

    const ExpEntryEx* probe(Stockfish::Key k);

    void defrag(int argc, char* argv[]);
    void merge(int argc, char* argv[]);
    void show_exp(Stockfish::Position& pos, bool extended);
    void convert_compact_pgn(int argc, char* argv[]);

    void pause_learning();
    void resume_learning();
    bool is_learning_paused();

    void add_pv_experience(Stockfish::Key k, Stockfish::Move m, Stockfish::Value v, Stockfish::Depth d);
    void add_multipv_experience(Stockfish::Key k, Stockfish::Move m, Stockfish::Value v, Stockfish::Depth d);
}

#endif


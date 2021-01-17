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

#include <unordered_map>
#include "types.h"

using namespace std;

#define MIN_EXP_DEPTH       4

namespace Experience
{
    struct ExpEntry
    {
        Key     key;        //8 bytes
        Move    move;       //4 bytes
        Value   value;      //4 bytes
        Depth   depth;      //4 bytes
        uint8_t padding[4]; //4 bytes

        ExpEntry() = delete;
        ExpEntry(const ExpEntry& exp) = delete;
        ExpEntry& operator =(const ExpEntry& exp) = delete;
        
        inline explicit ExpEntry(Key k, Move m, Value v, Depth d)
        {
            key = k;
            move = m;
            value = v;
            depth = d;
            padding[0] = padding[2] = 0x00;
            padding[1] = padding[3] = 0xFF;
        }

        inline int compare(const ExpEntry* exp) const
        {
            return (value * depth) - (exp->value * exp->depth);
        }

        inline void merge(const ExpEntry* exp)
        {
            assert(key == exp->key);
            assert(move == exp->move);

            if (depth > exp->depth)
                return;

            if (depth == exp->depth)
            {
                value = (value + exp->value) / 2;
            }
            else
            {
                value = exp->value;
                depth = exp->depth;
            }
        }
    };

    static_assert(sizeof(ExpEntry) == 24);

    //Experience structure
    struct ExpEntryEx : public ExpEntry
    {
        ExpEntryEx* next = nullptr;

        ExpEntryEx() = delete;
        ExpEntryEx(const ExpEntryEx& expEx) = delete;
        ExpEntryEx &operator =(const ExpEntryEx& expEx) = delete;

        inline ExpEntryEx* find(Move m)
        {
            ExpEntryEx* expEx = this;
            do
            {
                if (expEx->move == m)
                    return expEx;

                expEx = expEx->next;
            } while (expEx);

            return nullptr;
        }
    };

    void init();
    bool enabled();

    void unload();
    void save();
    void reload();

    void wait_for_loading_finished();

    const ExpEntryEx* probe(Key k);

    void defrag(int argc, char* argv[]);
    void merge(int argc, char* argv[]);

    void pause_learning();
    void resume_learning();
    bool is_learning_paused();

    void add_pv_experience(Key k, Move m, Value v, Depth d);
    void add_multipv_experience(Key k, Move m, Value v, Depth d);  
}

#endif


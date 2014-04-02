/*
 * This File is part of Davix, The IO library for HTTP based protocols
 * Copyright (C) 2013  Adrien Devresse <adrien.devresse@cern.ch>, CERN
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
*/


#ifndef DAVIX_TYPES_HPP
#define DAVIX_TYPES_HPP

#include <string>
#include <iostream>
#include <vector>
#include <stack>
#include <deque>
#include <typeinfo>
#include <algorithm>
#include <functional>
#include <davix_types.h>

#ifndef __DAVIX_INSIDE__
#error "Only davix.hpp should be included."
#endif





/// @class NonCopyable
/// @brief Simple NonCopyableProtector
class NonCopyable{
protected:
   NonCopyable() {}
    ~NonCopyable() {}
private:  // emphasize the following members are private
   NonCopyable( const NonCopyable& );
   const NonCopyable& operator=( const NonCopyable& );
};



#endif // DAVIX_TYPES_HPP
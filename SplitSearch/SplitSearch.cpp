/*
 *  Copyright (c) 2010-2017 Fabric Software Inc. All rights reserved.
 */

#include "SplitSearch.hpp"
#include <FTL/FS.h>
#include <FTL/JSONValue.h>

#include <ctype.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringMap.h>
#include <stdint.h>
#include <stdio.h>
#include <streambuf>
#include <string>
#include <vector>

namespace FabricServices { namespace SplitSearch { namespace Impl {

template<typename ArrayTy>
void SplitDelimitedString(
  llvm::StringRef delimitedStr,
  char delimiter,
  ArrayTy &result
  )
{
  std::pair<llvm::StringRef, llvm::StringRef> split(
    llvm::StringRef(), delimitedStr
    );
  while ( !split.second.empty() )
  {
    split = split.second.split( delimiter );
    result.push_back( split.first );
  }
}

static inline unsigned CommonSuffixLength(
  llvm::StringRef lhs,
  llvm::StringRef rhs
  )
{
  unsigned length = 0;
  for (;;)
  {
    if ( length >= lhs.size()
      || length >= rhs.size()
      || tolower( lhs[lhs.size()-length-1] )
        != tolower( rhs[rhs.size()-length-1] ) )
      break;
    ++length;
  }
  return length;
}

struct Score
{
  uint64_t points;
  uint64_t penalty;

  Score()
    : points( 0 )
    , penalty( 0 )
    {}

  Score( uint64_t _points, uint64_t _penalty )
    : points( _points ), penalty( _penalty ) {}

  bool isValid() const
    { return points != UINT64_MAX && penalty != UINT64_MAX; }

  static Score Invalid()
    { return Score( UINT64_MAX, UINT64_MAX ); }

  Score &operator+=( Score const &that )
  {
    points += that.points;
    penalty += that.penalty;
    return *this;
  }

  bool operator<( Score const &that ) const
  {
    return points < that.points
      || ( points == that.points
        && penalty > that.penalty );
  }

  bool operator>( Score const &that ) const
  {
    return points > that.points
      || ( points == that.points
        && penalty < that.penalty );
  }
};

struct RevMatchResult
{
  uint64_t size;
  Score score;

  RevMatchResult()
    : size( 0 )
    {}

  RevMatchResult &operator+=( RevMatchResult const &that )
  {
    size += that.size;
    score += that.score;
    return *this;
  }
};

inline uint64_t Sq( uint64_t x ) { return x * x; }

static inline RevMatchResult RevMatch(
  llvm::StringRef haystack,
  llvm::StringRef needle
  )
{
  RevMatchResult bestResult;
  bestResult.score.penalty = Sq( haystack.size() + 1 );
  uint64_t tail = 0;
  while ( !haystack.empty() )
  {
    RevMatchResult thisResult;
    thisResult.size = CommonSuffixLength( haystack, needle );
    if ( thisResult.size > 0 )
    {
      uint64_t head = haystack.size() - thisResult.size;
      thisResult.score.points = Sq(thisResult.size);
      thisResult.score.penalty = Sq(head + 1) + tail;
      if ( thisResult.size < haystack.size()
        && thisResult.size < needle.size() )
      {
        llvm::StringRef subHaystack(
          haystack.data(), haystack.size() - thisResult.size
          );
        llvm::StringRef subNeedle(
          needle.data(), needle.size() - thisResult.size
          );
        thisResult += RevMatch( subHaystack, subNeedle );
      }
      if ( bestResult.score < thisResult.score )
        bestResult = thisResult;
    }
    haystack = haystack.drop_back();
    ++tail;
  }
  return bestResult;
}

static inline Score ScoreMatch(
  llvm::ArrayRef<llvm::StringRef> prefixes,
  llvm::ArrayRef<llvm::StringRef> needle
  )
{
  if ( needle.empty() )
    return Score::Invalid();

  llvm::StringRef lastNeedle = needle.back();
  llvm::StringRef lastPrefix = prefixes.back();
  needle = needle.drop_back();
  RevMatchResult revMatch = RevMatch( lastPrefix, lastNeedle );

  Score subScore;
  llvm::StringRef subLastNeedle =
    lastNeedle.drop_back( revMatch.size );
  if ( !needle.empty() || !subLastNeedle.empty() )
  {
    if ( prefixes.size() > 1 )
    {
      llvm::SmallVector<llvm::StringRef, 8> subNeedle;
      subNeedle.append( needle.begin(), needle.end() );
      if ( !subLastNeedle.empty() )
        subNeedle.push_back( subLastNeedle );
      
      llvm::ArrayRef<llvm::StringRef> subPrefixes = prefixes.drop_back();
      subScore = ScoreMatch( subPrefixes, subNeedle );
    }
    else subScore = Score::Invalid();
  }

  if ( subScore.isValid() )
    return Score(
      revMatch.score.points + subScore.points/2,
      revMatch.score.penalty + subScore.penalty/2
      );
  else
    return Score::Invalid();
}

static inline llvm::ArrayRef<llvm::StringRef> DropFront(
  llvm::ArrayRef<llvm::StringRef> strs
  )
{
  return llvm::ArrayRef<llvm::StringRef>( strs.begin() + 1, strs.end() );
}

class Node;

class Match
{
  Node *m_node;
  void const *m_userdata;
  Score m_score;
  unsigned m_echelon;
  unsigned m_selectCount;

public:

  // Only exists for resize down
  Match()
    : m_node( 0 )
    , m_userdata( 0 )
    {}

  Match(
    Node *node,
    void const *userdata,
    Score score,
    unsigned echelon,
    unsigned selectCount
    )
    : m_node( node )
    , m_userdata( userdata )
    , m_score( score )
    , m_echelon( echelon )
    , m_selectCount( selectCount )
    {}

  Node *getNode() const { return m_node; }
  void const *getUserdata() const { return m_userdata; }

  void dump( size_t index )
  {
    printf( "index=%u score.points=%u score.penalty=%u echelon=%u selectCount=%u userdata=%s\n",
      unsigned( index ),
      unsigned( m_score.points ),
      unsigned( m_score.penalty ),
      m_echelon,
      m_selectCount,
      (char const *)m_userdata
      );
  }

  struct LessThan
  {
    bool operator()( Match const &lhs, Match const &rhs )
    {
      return lhs.m_echelon > rhs.m_echelon
        || ( lhs.m_echelon == rhs.m_echelon
          && ( lhs.m_selectCount > rhs.m_selectCount
            || ( lhs.m_selectCount == rhs.m_selectCount
              && lhs.m_score > rhs.m_score ) ) );
    }
  };
};

class Shareable
{
  unsigned _refCount;

protected:

  Shareable() : _refCount( 1 ) {}
  virtual ~Shareable() {}

public:

  void retain()
  {
    ++_refCount;
  }

  void release()
  {
    if ( --_refCount == 0 )
      delete this;
  }
};

class Matches : public Shareable
{
  std::vector<Match> m_impl;

  Matches( Matches const & ) = delete;
  Matches &operator=( Matches const & ) = delete;

protected:

  virtual ~Matches() {}

public:

  Matches() {}

  void add(
    Node *node,
    void const *userdata,
    Score score,
    unsigned echelon,
    unsigned selectCount
    )
  {
    m_impl.push_back( Match( node, userdata, score, echelon, selectCount ) );
  }

  void sort() { std::sort( m_impl.begin(), m_impl.end(), Match::LessThan() ); }

  void dump()
  {
    for ( size_t i = 0; i < m_impl.size(); ++i )
    {
      if ( i >= 20 )
        break;
      m_impl[i].dump( i );
    }
  }

  unsigned getSize() const { return m_impl.size(); }

  void const *getUserdata( unsigned index )
  {
    if ( index < m_impl.size() )
      return m_impl[index].getUserdata();
    else
      return NULL;
  }

  unsigned getUserdatas(
    unsigned max,
    void const **userdatas
    ) const
  {
    unsigned index = 0;
    while ( index < max && index < m_impl.size() )
    {
      userdatas[index] = m_impl[index].getUserdata();
      ++index;
    }
    return index;
  }

  void keepFirst( unsigned count )
    { m_impl.resize( std::min( m_impl.size(), size_t( count ) ) ); }

  Match const *getMatch( unsigned index ) const
    { return &m_impl[index]; }
};

class Dict;

class Node
{
  Dict *m_dict;
  void const *m_userdata;
  unsigned m_echelon;
  unsigned m_selectCount;
  llvm::StringMap< FTL::OwnedPtr<Node> > m_children;

protected:

  void search(
    llvm::SmallVector<llvm::StringRef, 8> &prefixes,
    llvm::ArrayRef<llvm::StringRef> needle,
    Matches *matches
    )
  {
    for ( llvm::StringMap< FTL::OwnedPtr<Node> >::iterator it =
      m_children.begin(); it != m_children.end(); ++it )
    {
      llvm::StringRef prefix = it->first();
      Node *node = it->second.get();

      prefixes.push_back( prefix );

      if ( node->m_userdata )
      {
        Score score = ScoreMatch( prefixes, needle );
        if ( score.isValid() )
          matches->add(
            node,
            node->m_userdata,
            score,
            node->m_echelon,
            node->m_selectCount
            );
      } 

      node->search( prefixes, needle, matches );

      prefixes.pop_back();
    }
  }

public:

  Node(
    Dict *dict,
    void *userdata,
    unsigned echelon,
    unsigned selectCount
    )
    : m_dict( dict )
    , m_userdata( userdata )
    , m_echelon( echelon )
    , m_selectCount( selectCount )
    {}
  Node( Node const & ) = delete;
  Node &operator=( Node const & ) = delete;
  ~Node() {}

  Dict *getDict() const
    { return m_dict; }

  bool add(
    llvm::ArrayRef<llvm::StringRef> strs,
    void const *userdata,
    unsigned echelon,
    unsigned selectCount
    )
  {
    if ( !strs.empty() )
    {
      FTL::OwnedPtr<Node> &child = m_children[strs.front()];
      if ( !child )
        child = new Node( m_dict, nullptr, echelon, selectCount );
      return child->add( DropFront( strs ), userdata, echelon, selectCount );
    }
    else
    {
      if ( !m_userdata )
        m_userdata = userdata;
      m_echelon = std::max( m_echelon, echelon );
      m_selectCount = std::max( m_selectCount, selectCount );
      return m_userdata == userdata;
    }
  }

  bool remove(
    llvm::ArrayRef<llvm::StringRef> strs,
    void const *userdata
    )
  {
    if ( !strs.empty() )
    {
      FTL::OwnedPtr<Node> &child = m_children[strs.front()];
      if ( !child )
        return false;
      return child->remove( DropFront( strs ), userdata );
    }
    else
    {
      bool result = m_userdata == userdata;
      m_userdata = nullptr;
      return result;
    }
  }

  void incSelectCount()
    { ++m_selectCount; }

  void clear()
  {
    m_children.clear();
  }

  void search(
    llvm::ArrayRef<llvm::StringRef> needle,
    Matches *matches
    )
  {
    llvm::SmallVector<llvm::StringRef, 8> prefixes;
    search( prefixes, needle, matches );
  }

  void loadPrefsFromJSON( FTL::JSONObject const *jsonObject )
  {
    m_selectCount = jsonObject->getSInt32OrDefault( FTL_STR("selectCount"), 0 );
    if ( FTL::JSONObject const *childJSONObject = jsonObject->maybeGetObject( FTL_STR("children") ) )
    {
      for ( FTL::JSONObject::const_iterator it = childJSONObject->begin();
        it != childJSONObject->end(); ++it )
      {
        FTL::StrRef childName = it->key();
        if ( FTL::JSONObject const *childJSONObject = it->value()->maybeCastOrNull<FTL::JSONObject>() )
        {
          llvm::StringMap< FTL::OwnedPtr<Node> >::iterator jt =
            m_children.find( llvm::StringRef( childName.data(), childName.size() ) );
          if ( jt != m_children.end() )
            jt->second->loadPrefsFromJSON( childJSONObject );
        }
      }
    }
  }

  FTL::JSONObject *savePrefsToJSON()
  {
    FTL::OwnedPtr<FTL::JSONObject> childrenJSONObject( new FTL::JSONObject );
    for ( llvm::StringMap< FTL::OwnedPtr<Node> >::iterator it =
      m_children.begin(); it != m_children.end(); ++it )
    {
      FTL::OwnedPtr<FTL::JSONObject> childPrefs( it->second->savePrefsToJSON() );
      if ( !childPrefs->empty() )
        childrenJSONObject->insert(
          FTL::StrRef( it->first().data(), it->first().size() ),
          childPrefs.take()
          );
    }

    FTL::JSONObject *resultJSONObject = new FTL::JSONObject;
    if ( m_selectCount != 0 )
      resultJSONObject->insert( FTL_STR("selectCount"), new FTL::JSONSInt32( m_selectCount ) );
    if ( !childrenJSONObject->empty() )
      resultJSONObject->insert( FTL_STR("children"), childrenJSONObject.take() );
    return resultJSONObject;
  }
};

class Dict : public Shareable
{
  Node m_root;

  Dict( Dict const & ) = delete;
  Dict &operator=( Dict const & ) = delete;

protected:

  virtual ~Dict() {}

public:

  Dict() : m_root( this, nullptr, 0, 0 ) {}

  bool add(
    llvm::ArrayRef<llvm::StringRef> strs,
    void const *userdata,
    unsigned echelon,
    unsigned selectCount
    )
  {
    return m_root.add( strs, userdata, echelon, selectCount );
  }

  bool remove(
    llvm::ArrayRef<llvm::StringRef> strs,
    void const *userdata
    )
  {
    return m_root.remove( strs, userdata );
  }

  void clear()
  {
    m_root.clear();
  }

  Matches *search( llvm::ArrayRef<llvm::StringRef> needle )
  {
    if ( needle.empty() )
      return nullptr;

    Matches *matches = new Matches;
    m_root.search( needle, matches );
    matches->sort();
    // matches->dump();
    return matches;
  }

  void loadPrefs( char const *filename )
  {
    if ( FTL::FSExists( filename ) )
    {
      try
      {
        std::ifstream file( filename );
        std::string jsonStr = std::string(
          std::istreambuf_iterator<char>( file ),
          std::istreambuf_iterator<char>()
          );
        FTL::JSONStrWithLoc jsonStrWithLoc( jsonStr );
        for (;;)
        {
          try
          {
            FTL::OwnedPtr<FTL::JSONValue> jsonValue(
              FTL::JSONValue::Decode( jsonStrWithLoc )
              );
            if ( !jsonValue )
              break;
            FTL::JSONObject const *jsonObject =
              jsonValue->cast<FTL::JSONObject>();
            if ( FTL::JSONValue const *nodesJSONValue =
              jsonObject->maybeGet( FTL_STR("nodes") ) )
              if ( FTL::JSONObject const *nodesJSONObject =
                nodesJSONValue->maybeCastOrNull<FTL::JSONObject>() )
                m_root.loadPrefsFromJSON( nodesJSONObject );
          }
          catch ( FTL::JSONException e )
          {
            std::cerr
              << "'" << filename << "': Caught exception: "
              << e.getDesc()
              << "\n";
          }
        }
      }
      catch ( ... )
      {
        std::cerr << "'" << filename << "': Unable to load";
      }
    }
  }

  void savePrefs( char const *filename )
  {
    try
    {
      FTL::OwnedPtr<FTL::JSONObject> prefs( new FTL::JSONObject );
      prefs->insert( FTL_STR("nodes"), m_root.savePrefsToJSON() );

      std::ofstream outFile( filename );
      outFile << prefs->encode() << '\n';
    }
    catch ( ... )
    {
      std::cerr << "'" << filename << "': Unable to save";
    }
  }
};

} } }

using namespace FabricServices::SplitSearch::Impl;

FABRICSERVICES_SPLITSEARCH_DECL
void FabricServices_SplitSearch_Matches_Retain(
  FabricServices_SplitSearch_Matches _matches
  )
{
  Matches *matches = static_cast<Matches *>( _matches );
  matches->retain();
}

FABRICSERVICES_SPLITSEARCH_DECL
void FabricServices_SplitSearch_Matches_Release(
  FabricServices_SplitSearch_Matches _matches
  )
{
  Matches *matches = static_cast<Matches *>( _matches );
  matches->release();
}

FABRICSERVICES_SPLITSEARCH_DECL
unsigned FabricServices_SplitSearch_Matches_GetSize(
  FabricServices_SplitSearch_Matches _matches
  )
{
  Matches *matches = static_cast<Matches *>( _matches );
  return matches->getSize();
}

FABRICSERVICES_SPLITSEARCH_DECL
const void *FabricServices_SplitSearch_Matches_GetUserdata(
  FabricServices_SplitSearch_Matches _matches,
  unsigned index
  )
{
  Matches *matches = static_cast<Matches *>( _matches );
  if ( index >= matches->getSize() )
  {
    std::cerr << "SplitSearch.Matches.getUserdata: index out of range\n";
    return 0;
  }
  else return matches->getUserdata( index );
}

FABRICSERVICES_SPLITSEARCH_DECL
unsigned FabricServices_SplitSearch_Matches_GetUserdatas(
  FabricServices_SplitSearch_Matches _matches,
  unsigned max,
  void const **userdatas
  )
{
  Matches *matches = static_cast<Matches *>( _matches );
  return matches->getUserdatas( max, userdatas );
}

FABRICSERVICES_SPLITSEARCH_DECL
void FabricServices_SplitSearch_Matches_Select(
  FabricServices_SplitSearch_Matches _matches,
  unsigned index
  )
{
  Matches const *matches = static_cast<Matches const *>( _matches );
  if ( index >= matches->getSize() )
    std::cerr << "SplitSearch.Matches.select: index out of range\n";
  else
  {
    Match const *match = matches->getMatch( index );
    Node *node = match->getNode();
    node->incSelectCount();
  }
}

FABRICSERVICES_SPLITSEARCH_DECL
void FabricServices_SplitSearch_Matches_KeepFirst(
  FabricServices_SplitSearch_Matches _matches,
  unsigned count
  )
{
  Matches *matches = static_cast<Matches *>( _matches );
  matches->keepFirst( count );
}

FABRICSERVICES_SPLITSEARCH_DECL
FabricServices_SplitSearch_Dict FabricServices_SplitSearch_Dict_Create(
  )
{
  return new Dict;
}

FABRICSERVICES_SPLITSEARCH_DECL
bool FabricServices_SplitSearch_Dict_Add(
  FabricServices_SplitSearch_Dict _dict,
  unsigned numCStrs,
  char const * const *cStrs,
  void const *userdata,
  unsigned echelon,
  unsigned selectCount
  )
{
  Dict *dict = static_cast<Dict *>( _dict );
  llvm::SmallVector<llvm::StringRef, 8> strs;
  while ( numCStrs-- > 0 )
    strs.push_back( *cStrs++ );
  return dict->add( strs, userdata, echelon, selectCount );
}

FABRICSERVICES_SPLITSEARCH_DECL
bool FabricServices_SplitSearch_Dict_Add_Delimited(
  FabricServices_SplitSearch_Dict _dict,
  char const *delimitedCStr,
  char delimiter,
  void const *userdata,
  unsigned echelon,
  unsigned selectCount
  )
{
  Dict *dict = static_cast<Dict *>( _dict );
  llvm::SmallVector<llvm::StringRef, 8> strs;
  SplitDelimitedString( delimitedCStr, delimiter, strs );
  return dict->add( strs, userdata, echelon, selectCount );
}

FABRICSERVICES_SPLITSEARCH_DECL
bool FabricServices_SplitSearch_Dict_Remove(
  FabricServices_SplitSearch_Dict _dict,
  unsigned numCStrs,
  char const * const *cStrs,
  void const *userdata
  )
{
  Dict *dict = static_cast<Dict *>( _dict );
  llvm::SmallVector<llvm::StringRef, 8> strs;
  while ( numCStrs-- > 0 )
    strs.push_back( *cStrs++ );
  return dict->remove( strs, userdata );
}

FABRICSERVICES_SPLITSEARCH_DECL
bool FabricServices_SplitSearch_Dict_Remove_Delimited(
  FabricServices_SplitSearch_Dict _dict,
  char const *delimitedCStr,
  char delimiter,
  void const *userdata
  )
{
  Dict *dict = static_cast<Dict *>( _dict );
  llvm::SmallVector<llvm::StringRef, 8> strs;
  SplitDelimitedString( delimitedCStr, delimiter, strs );
  return dict->remove( strs , userdata );
}

FABRICSERVICES_SPLITSEARCH_DECL
void FabricServices_SplitSearch_Dict_Clear(
  FabricServices_SplitSearch_Dict _dict
  )
{
  Dict *dict = static_cast<Dict *>( _dict );
  dict->clear();
}

FABRICSERVICES_SPLITSEARCH_DECL
void FabricServices_SplitSearch_Dict_LoadPrefs(
  FabricServices_SplitSearch_Dict _dict,
  char const *filename
  )
{
  Dict *dict = static_cast<Dict *>( _dict );
  dict->loadPrefs( filename );
}

FABRICSERVICES_SPLITSEARCH_DECL
void FabricServices_SplitSearch_Dict_SavePrefs(
  FabricServices_SplitSearch_Dict _dict,
  char const *filename
  )
{
  Dict *dict = static_cast<Dict *>( _dict );
  dict->savePrefs( filename );
}

FABRICSERVICES_SPLITSEARCH_DECL
FabricServices_SplitSearch_Matches FabricServices_SplitSearch_Dict_Search(
  FabricServices_SplitSearch_Dict _dict,
  unsigned numCStrs,
  char const * const *cStrs
  )
{
  Dict *dict = static_cast<Dict *>( _dict );

  // llvm::StringRef testHaystack[] = {
  //   "Mat44",
  //   "MultiplyVector3"
  // };
  // llvm::StringRef testNeedle[] = {
  //   "mat4mul"
  // };
  // Score testScore = ScoreMatch( testHaystack, testNeedle );
  // (void)testScore;

  llvm::SmallVector<llvm::StringRef, 8> needle;
  for ( unsigned i = 0; i < numCStrs; ++i )
    needle.push_back( cStrs[i] );
  return dict->search( needle );
}

FABRICSERVICES_SPLITSEARCH_DECL
void FabricServices_SplitSearch_Dict_Retain(
  FabricServices_SplitSearch_Dict _dict
  )
{
  Dict *dict = static_cast<Dict *>( _dict );
  dict->retain();
}

FABRICSERVICES_SPLITSEARCH_DECL
void FabricServices_SplitSearch_Dict_Release(
  FabricServices_SplitSearch_Dict _dict
  )
{
  Dict *dict = static_cast<Dict *>( _dict );
  dict->release();
}

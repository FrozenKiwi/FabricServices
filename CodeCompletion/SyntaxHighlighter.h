// Copyright 2010-2014 Fabric Engine Inc. All rights reserved.

#ifndef __CodeCompletion_SyntaxHighlighter__
#define __CodeCompletion_SyntaxHighlighter__

#include "HighlightRule.h"
#include <vector>

namespace FabricServices
{

  namespace CodeCompletion
  {

    class SyntaxHighlighter
    {

    public:

      struct Format
      {
        HighlightRuleType type;
        uint32_t start;
        uint32_t length;
        const char * prefix;
        const char * suffix;
      };

      SyntaxHighlighter();
      virtual ~SyntaxHighlighter();

      // rule management
      HighlightRule * addRule(HighlightRuleType type, const std::string & pattern, const std::string & formatPrefix = "", const std::string & formatSuffix = "");
      uint32_t getRuleCount() const;
      const HighlightRule * getRule(uint32_t index) const;
      const char * getRuleTypeName(HighlightRuleType type) const;

      // highlighting
      virtual const std::vector<Format> & getHighlightFormats(const std::string & text) const;
      virtual std::string getHighlightedText(const std::string & text) const;

    private:

      std::vector<HighlightRule*> m_rules;
      mutable std::string m_lastText;
      mutable std::vector<Format> m_lastFormats;
    };

  };

};


#endif // __CodeCompletion_SyntaxHighlighter__

#include "blink/korean_input.h"
#include <cassert>
#include <iostream>
using uefi_browser::KoreanInput;
static std::u16string compose(const char* keys) {
  KoreanInput input;
  std::u16string committed;
  while(*keys) { auto result=input.Feed(*keys++); assert(result.handled); committed+=result.committed; }
  return committed+input.Preedit();
}
int main() {
  assert(compose("gksrmf")==u"한글");
  assert(compose("dkssudgktpdy")==u"안녕하세요");
  assert(compose("rkqt")==u"값");
  assert(compose("rkqtdl")==u"값이");
  assert(compose("rkqtk")==u"갑사");
  assert(compose("dlfrdj")==u"읽어");
  assert(compose("dlfrk")==u"일가");
  assert(compose("RkRk")==u"까까");
  assert(compose("rhksrP")==u"관계");
  assert(compose("npl")==u"ㅞㅣ");
  const char* initials[] = {"r","R","s","e","E","f","a","q","Q","t","T","d","w","W","c","z","x","v","g"};
  const char* vowels[] = {"k","o","i","O","j","p","u","P","h","hk","ho","hl","y","n","nj","np","nl","b","m","ml","l"};
  const char* finals[] = {"","r","R","rt","s","sw","sg","e","f","fr","fa","fq","ft","fx","fv","fg","a","q","qt","t","T","d","w","c","z","x","v","g"};
  for (int initial=0;initial<19;++initial)
    for (int vowel=0;vowel<21;++vowel)
      for (int final=0;final<28;++final) {
        std::string keys=std::string(initials[initial])+vowels[vowel]+finals[final];
        assert(compose(keys.c_str())==std::u16string(1,char16_t(0xac00+(initial*21+vowel)*28+final)));
      }
  KoreanInput input;
  for(char key:std::string("rhks"))input.Feed(key);
  assert(input.Preedit()==u"관");
  assert(input.Backspace().preedit==u"과");
  assert(input.Backspace().preedit==u"고");
  assert(input.Backspace().preedit==u"ㄱ");
  assert(input.Backspace().preedit.empty());
  assert(!input.Backspace().handled && !input.Feed('1').handled);
  for(char key:std::string("rkqtk"))input.Feed(key);
  assert(input.Preedit()==u"사" && input.Backspace().preedit==u"ㅅ");
  input.Clear();
  assert(input.Preedit().empty());
  KoreanInput other;
  input.Feed('r'); other.Feed('s');
  assert(input.Feed('k').preedit==u"가");
  assert(other.Feed('k').preedit==u"나");
  input.Clear();
  assert(other.Preedit()==u"나" && input.Feed('g').preedit==u"ㅎ");
  std::cout<<"PASS: all 11,172 modern Hangul syllables, Korean two-set composition, compound finals/vowels, resyllabification and keystroke deletion\n";
}

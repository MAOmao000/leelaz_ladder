#ifndef _PATTERN_H_
#define _PATTERN_H_

const int MD2_MAX = 16777216;	// 2^24
const int PAT3_MAX = 65536;	// 2^16

const int MD2_LIMIT = 1060624;
const int PAT3_LIMIT = 4468;

enum MD {
  MD_2,
  MD_3,
  MD_4,
  MD_MAX
};

enum LARGE_MD {
  MD_5,
  MD_LARGE_MAX
};

//////////////
//    //
//////////////

//  
struct pattern_t {
  unsigned int list[MD_MAX];
  unsigned long long large_list[MD_LARGE_MAX];
};


////////////
//    //
////////////

//  
void ClearPattern( pattern_t *pat );

//  
void UpdatePat3Empty( pattern_t *pat, const int pos );
void UpdatePat3Stone( pattern_t *pat, const int color, const int pos );
void UpdateMD2Empty( pattern_t *pat, const int pos );
void UpdateMD2Stone( pattern_t *pat, const int color, const int pos );
void UpdatePatternEmpty( pattern_t *pat, const int pos );
void UpdatePatternStone( pattern_t *pat, const int color, const int pos );

//  
void Pat3Transpose8( const unsigned int pat3, unsigned int *transp );
void Pat3Transpose16( const unsigned int pat3, unsigned int *transp );
void MD2Transpose8( const unsigned int md2, unsigned int *transp );
void MD2Transpose16( const unsigned int md2, unsigned int *transp );
void MD3Transpose8( const unsigned int md3, unsigned int *transp );
void MD3Transpose16( const unsigned int md3, unsigned int *transp );
void MD4Transpose8( const unsigned int md4, unsigned int *transp );
void MD4Transpose16( const unsigned int md4, unsigned int *transp );
void MD5Transpose8( const unsigned long long md5, unsigned long long *transp );
void MD5Transpose16( const unsigned long long md5, unsigned long long *transp );

//  
unsigned int Pat3Reverse( const unsigned int pat3 );
unsigned int MD2Reverse( const unsigned int md2 );
unsigned int MD3Reverse( const unsigned int md3 );
unsigned int MD4Reverse( const unsigned int md4 );
unsigned long long MD5Reverse( const unsigned long long md5 );

//  
unsigned int Pat3VerticalMirror( const unsigned int pat3 );
unsigned int MD2VerticalMirror( const unsigned int md2 );
unsigned int MD3VerticalMirror( const unsigned int md3 );
unsigned int MD4VerticalMirror( const unsigned int md4 );
unsigned long long MD5VerticalMirror( const unsigned long long md5 );

//  
unsigned int Pat3HorizontalMirror( const unsigned int pat3 );
unsigned int MD2HorizontalMirror( const unsigned int md2 );
unsigned int MD3HorizontalMirror( const unsigned int md3 );
unsigned int MD4HorizontalMirror( const unsigned int md4 );
unsigned long long MD5HorizontalMirror( const unsigned long long md5 );

//  90
unsigned int Pat3Rotate90( const unsigned int pat3 );
unsigned int MD2Rotate90( const unsigned int md2 );
unsigned int MD3Rotate90( const unsigned int md3 );
unsigned int MD4Rotate90( const unsigned int md4 );
unsigned long long MD5Rotate90( const unsigned long long md5 );

//  
unsigned int Pat3( const pattern_t *pat, const int pos );
unsigned int MD2( const pattern_t *pat, const int pos );
unsigned int MD3( const pattern_t *pat, const int pos );
unsigned int MD4( const pattern_t *pat, const int pos );
unsigned long long MD5( const pattern_t *pat, const int pos );

//  
void DisplayInputPat3( const unsigned int pat3 );
void DisplayInputMD2( const unsigned int md2 );
void DisplayInputMD3( const unsigned int md3 );
void DisplayInputMD4( const unsigned int md4 );
void DisplayInputMD5( const unsigned long long md5 );
void DisplayInputPattern( const pattern_t *pattern, const int size );

#endif	//  _PATTERN_H_

#include "DataDef.h"
#include "GeoBase/GB_Math.h"
#include "GeoBase/GB_Crypto.h"

std::string MapTile::CalculateUid() const
{
	const std::string randomString = GB_RandomString(32);
	return GB_Md5Hash(randomString);
}






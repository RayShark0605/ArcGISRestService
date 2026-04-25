#ifndef GEOBASE_UTILITY_H_H
#define GEOBASE_UTILITY_H_H

#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <queue>
#include <sstream>
#include <fstream>
#include <chrono>
#include <thread>

#include "../ArcGISRestServicePort.h"

// 获取当前控制台编码
ARCGIS_RESTSERVICE_PORT void GB_GetConsoleEncodingString(std::string& encodingString);
ARCGIS_RESTSERVICE_PORT void GB_GetConsoleEncodingCode(unsigned int& codePageId); // 未知编码则返回UINT_MAX

// 设置控制台编码
ARCGIS_RESTSERVICE_PORT bool GB_SetConsoleEncoding(unsigned int codePageId);

// 设置控制台编码为 UTF-8
ARCGIS_RESTSERVICE_PORT bool GB_SetConsoleEncodingToUtf8();


#endif
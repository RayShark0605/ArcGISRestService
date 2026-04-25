#pragma once

#if defined(ARCGIS_RESTSERVICE_EXPORTS)
#define ARCGIS_RESTSERVICE_PORT __declspec(dllexport)
#else
#define ARCGIS_RESTSERVICE_PORT __declspec(dllimport)
#endif


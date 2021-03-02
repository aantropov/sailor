#pragma once

#ifndef _SAILOR_IMPORT_
# define SAILOR_API __declspec(dllexport)
#else
# define SAILOR_API __declspec(dllimport)
#endif
/* Minimal AConfiguration for the iOS port: only the screen density is used. */
#ifndef IOS_ANDROID_CONFIGURATION_H
#define IOS_ANDROID_CONFIGURATION_H
#include <stdint.h>
#include "asset_manager.h"

typedef struct AConfiguration AConfiguration;

AConfiguration *AConfiguration_new(void);
void AConfiguration_delete(AConfiguration *config);
void AConfiguration_fromAssetManager(AConfiguration *out, AAssetManager *assetManager);
int32_t AConfiguration_getDensity(AConfiguration *config);

#endif

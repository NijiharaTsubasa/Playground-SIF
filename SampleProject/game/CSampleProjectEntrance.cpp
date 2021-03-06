﻿/* 
   Copyright 2013 KLab Inc.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include <stdlib.h>
#include "CSampleProjectEntrance.h"

bool
GameSetup()
{
	CSampleProjectEntrance * pClient = CSampleProjectEntrance::getInstance();

	CPFInterface& pfif = CPFInterface::getInstance();
	pfif.setClientRequest(pClient);
	klb_assert(pfif.platform().registerFont("MotoyaLMaru W3 mono", "asset://MTLmr3m.ttf", true), "[LoveLive base] GAME FONT MTLmr3m.ttf NOT INSTALLED");
	return true;
}

CSampleProjectEntrance::CSampleProjectEntrance() : CKLBGameApplication() {}
CSampleProjectEntrance::~CSampleProjectEntrance() {}

CSampleProjectEntrance *
CSampleProjectEntrance::getInstance()
{
    static CSampleProjectEntrance instance;
    return &instance;
}


bool
CSampleProjectEntrance::initLocalSystem(CKLBAssetManager& mgrAsset)
{
	return true;
}

void
CSampleProjectEntrance::localFinish()
{
}

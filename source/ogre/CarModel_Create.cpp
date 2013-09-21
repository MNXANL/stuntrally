#include "pch.h"
#include "common/Def_Str.h"
#include "../vdrift/pathmanager.h"
#include "../vdrift/mathvector.h"
#include "../vdrift/track.h"
#include "../vdrift/game.h"
#include "../vdrift/performance_testing.h"
#include "common/data/SceneXml.h"
#include "common/RenderConst.h"
#include "CGame.h"
#include "CGui.h"
#include "CarModel.h"
#include "SplitScreen.h"
#include "FollowCamera.h"
#include "CarReflection.h"
#include "../road/Road.h"
#include "../shiny/Main/Factory.hpp"
#include "../network/gameclient.hpp"
#include <OgreRoot.h>
#include <OgreTerrain.h>
#include <OgreEntity.h>
#include <OgreManualObject.h>
#include <OgreMaterialManager.h>
#include <OgreParticleSystem.h>
#include <OgreParticleEmitter.h>
#include <OgreParticleAffector.h>
#include <OgreRibbonTrail.h>
using namespace Ogre;
#define  FileExists(s)  PATHMANAGER::FileExists(s)


//  ctor
//------------------------------------------------------------------------------------------------------
CarModel::CarModel(int index, int colorId, eCarType type, const std::string& name,
	SceneManager* sceneMgr, SETTINGS* set, GAME* game, Scene* s, Camera* cam, App* app)
	:mSceneMgr(sceneMgr), pSet(set), pGame(game), sc(s), mCamera(cam), pApp(app)
	,iIndex(index), iColor(colorId % 6), sDirname(name), eType(type)
	,fCam(0), pMainNode(0), pCar(0), terrain(0), ndSph(0), brakes(0)
	,pReflect(0), color(0,1,0)
	,hideTime(1.f), mbVisible(true), bLightMapEnabled(true), bBraking(false)
	,iCamNextOld(0), bLastChkOld(0), bWrongChk(0),  iFirst(0)
	,angCarY(0), vStartPos(0,0,0), pNickTxt(0)
	,ndNextChk(0), entNextChk(0)
	,all_subs(0), all_tris(0)  //stats
	,bGetStPos(true), fChkTime(0.f), iWonPlace(0), iWonPlaceOld(0)
	,iCurChk(-1), iNumChks(0), iNextChk(0)  //ResetChecks();  // road isnt yet
	,timeAtCurChk(0.f)
	,distFirst(1.f), distLast(1.f), distTotal(10.f), trackPercent(0.f), updTimes(1)
{
	for (int w = 0; w < 4; ++w)
	{
		for (int p=0; p < PAR_ALL; ++p)
			par[p][w] = 0;

		ndWh[w] = 0;  ndWhE[w] = 0;  whTrail[w] = 0;  ndBrake[w] = 0;
		whTemp[w] = 0.f;  whWidth[w] = 0.2f;
	}
	for (int i=0; i < 2; i++)
		parBoost[i] = 0;
	parHit = 0;

	qFixWh[0].Rotate(2*PI_d,0,0,1);
	qFixWh[1].Rotate(  PI_d,0,0,1);

	Defaults();
}

void CarModel::Defaults()
{
	for (int i=0; i<3; ++i)
	{
		driver_view[i] = 0.f;  hood_view[i] = 0.f;
		interiorOffset[i] = 0.f;  boostOffset[i] = 0.f;  exhaustPos[i] = 0.f;
	}
	brakePos.clear();
	brakeClr = ColourValue(1,0,0);
	brakeSize = 0.f;

	bRotFix = false;
	sBoostParName = "Boost";  boostSizeZ = 1.f;

	for (int w=0; w<4; ++w)
	{
		whRadius[w] = 0.3f;  whWidth[w] = 0.2f;
	}
	manualExhaustPos = false;  has2exhausts = false;
}

//  Load CAR
//------------------------------------------------------------------------------------------------------
void CarModel::Load(int startId)
{
	//  names for local play
	if (isGhostTrk())    sDispName = TR("#{Track}");
	else if (isGhost())  sDispName = TR("#{Ghost}");
	else if (eType == CT_LOCAL)
		sDispName = TR("#{Player}") + toStr(iIndex+1);
	

	///  load config .car
	std::string pathCar;
	pApp->gui->GetCarPath(&pathCar, 0, 0, sDirname, pApp->mClient);  // force orig for newtorked games
	LoadConfig(pathCar);
	
	
	///  Create CAR (dynamic)
	if (!isGhost())  // ghost has pCar, dont create
	{
		if (startId == -1)  startId = iIndex;
		if (pSet->game.start_order == 1)
		{	//  reverse start order
			int numCars = pApp->mClient ? pApp->mClient->getPeerCount()+1 : pSet->game.local_players;  // networked or splitscreen
			startId = numCars-1 - startId;
		}
		int i = pSet->game.collis_cars ? startId : 0;  // offset when cars collide

		MATHVECTOR<float,3> pos(0,10,0);  pos = pGame->track.GetStart(i).first;
		QUATERNION<float> rot;  rot = pGame->track.GetStart(i).second;
		vStartPos = Vector3(pos[0], pos[2], -pos[1]);

		pCar = pGame->LoadCar(pathCar, sDirname, pos, rot, true, false, eType == CT_REMOTE, iIndex);

		if (!pCar)  LogO("Error creating car " + sDirname + "  path: " + pathCar);
		else  pCar->pCarM = this;
	}
}

//  Destroy
//------------------------------------------------------------------------------------------------------
CarModel::~CarModel()
{
	delete pReflect;  pReflect = 0;
	
	delete fCam;  fCam = 0;
	
	//  hide trails
	for (int w=0; w<4; ++w)  if (whTrail[w]) {  whTemp[w] = 0.f;
		whTrail[w]->setVisible(false);	whTrail[w]->setInitialColour(0, 0.5,0.5,0.5, 0);	}

	//  destroy cloned materials
	for (int i=0; i<NumMaterials; ++i)
		MaterialManager::getSingleton().remove(sMtr[i]);
	
	//  destroy par sys
	for (int w=0; w < 4; ++w)
	{	for (int p=0; p < PAR_ALL; ++p)
			if (par[p][w]) {  mSceneMgr->destroyParticleSystem(par[p][w]);  par[p][w]=0;  }
		if (ndBrake[w])  mSceneMgr->destroySceneNode(ndBrake[w]);
	}
	for (int i=0; i < 2; i++)
		if (parBoost[i]) {  mSceneMgr->destroyParticleSystem(parBoost[i]);  parBoost[i]=0;  }
	if (parHit) {  mSceneMgr->destroyParticleSystem(parHit);  parHit=0;  }
						
	if (brakes)  mSceneMgr->destroyBillboardSet(brakes);
	if (pMainNode)  mSceneMgr->destroySceneNode(pMainNode);
	
	//  destroy resource group, will also destroy all resources in it
	if (ResourceGroupManager::getSingleton().resourceGroupExists(resGrpId))
		ResourceGroupManager::getSingleton().destroyResourceGroup(resGrpId);
}


///   Load .car
//------------------------------------------------------------------------------------------------------
static void ConvertV2to1(float & x, float & y, float & z)
{
	float tx = x, ty = y, tz = z;
	x = ty;  y = -tx;  z = tz;
}
void CarModel::LoadConfig(const std::string & pathCar)
{
	Defaults();

	///  load  -----
	CONFIGFILE cf;
	if (!cf.Load(pathCar))
	{  LogO("!! CarModel: Can't load .car "+pathCar);  return;  }


	//-  custom interior model offset
	cf.GetParam("model_ofs.interior-x", interiorOffset[0]);
	cf.GetParam("model_ofs.interior-y", interiorOffset[1]);
	cf.GetParam("model_ofs.interior-z", interiorOffset[2]);
	cf.GetParam("model_ofs.rot_fix", bRotFix);

	//~  boost offset
	cf.GetParam("model_ofs.boost-x", boostOffset[0]);
	cf.GetParam("model_ofs.boost-y", boostOffset[1]);
	cf.GetParam("model_ofs.boost-z", boostOffset[2]);
	cf.GetParam("model_ofs.boost-size-z", boostSizeZ);
	cf.GetParam("model_ofs.boost-name", sBoostParName);

	//~  brake flares
	float pos[3];  bool ok=true;  int i=0;
	while (ok)
	{	ok = cf.GetParam("flares.brake-pos"+toStr(i), pos);  ++i;
		if (ok)  brakePos.push_back(bRotFix ? Vector3(-pos[0],pos[2],pos[1]) : Vector3(-pos[1],-pos[2],pos[0]));
	}
	cf.GetParam("flares.brake-color", pos);
	brakeClr = ColourValue(pos[0],pos[1],pos[2]);
	cf.GetParam("flares.brake-size", brakeSize);
	
	
	//-  custom exhaust pos for boost particles
	if (cf.GetParam("model_ofs.exhaust-x", exhaustPos[0]))
	{
		manualExhaustPos = true;
		cf.GetParam("model_ofs.exhaust-y", exhaustPos[1]);
		cf.GetParam("model_ofs.exhaust-z", exhaustPos[2]);
	}else
		manualExhaustPos = false;
	if (!cf.GetParam("model_ofs.exhaust-mirror-second", has2exhausts))
		has2exhausts = false;


	//- load cameras pos
	cf.GetParam("driver.view-position", pos, pGame->error_output);
	driver_view[0]=pos[1]; driver_view[1]=-pos[0]; driver_view[2]=pos[2];
	
	cf.GetParam("driver.hood-position", pos, pGame->error_output);
	hood_view[0]=pos[1]; hood_view[1]=-pos[0]; hood_view[2]=pos[2];


	//  tire params
	WHEEL_POSITION leftside = FRONT_LEFT, rightside = FRONT_RIGHT;
	float value;
	bool both = cf.GetParam("tire-both.radius", value);
	std::string posstr = both ? "both" : "front";

	for (int p = 0; p < 2; ++p)
	{
		if (p == 1)
		{
			leftside = REAR_LEFT;
			rightside = REAR_RIGHT;
			if (!both)  posstr = "rear";
		}
		float radius;
		cf.GetParam("tire-"+posstr+".radius", radius, pGame->error_output);
		whRadius[leftside] = radius;
		whRadius[rightside] = radius;
		
		float width = 0.2f;
		cf.GetParam("tire-"+posstr+".width-trail", width);
		whWidth[leftside] = width;
		whWidth[rightside] = width;
	}
	
	//  wheel pos
	//  for track's ghost or garage view
	int version(1);
	cf.GetParam("version", version);
	for (int i = 0; i < 4; ++i)
	{
		std::string sPos;
		if (i == 0)			sPos = "FL";
		else if (i == 1)	sPos = "FR";
		else if (i == 2)	sPos = "RL";
		else				sPos = "RR";

		float pos[3];
		MATHVECTOR<float,3> vec;

		cf.GetParam("wheel-"+sPos+".position", pos, pGame->error_output);
		if (version == 2)  ConvertV2to1(pos[0],pos[1],pos[2]);
		vec.Set(pos[0],pos[1], pos[2]);
		whPos[i] = vec;
	}
	//  steer angle
	maxangle = 26.f;
	cf.GetParam("steering.max-angle", maxangle, pGame->error_output);
	maxangle *= pGame->GetSteerRange();
}

	
//  log mesh stats
void CarModel::LogMeshInfo(const Entity* ent, const String& name, int mul)
{
	//return;
	const MeshPtr& msh = ent->getMesh();
	int tris=0, subs = msh->getNumSubMeshes();
	for (int i=0; i < subs; ++i)
	{
		SubMesh* sm = msh->getSubMesh(i);
		tris += sm->indexData->indexCount;
	}
	all_tris += tris * mul;  //wheels x4
	all_subs += subs * mul;
	LogO("MESH info:  "+name+"\t sub: "+toStr(subs)+"  tri: "+fToStr(tris/1000.f,1,4)+"k");
}

//  CreatePart mesh
//---------------------------------------------------
void CarModel::CreatePart(SceneNode* ndCar, Vector3 vPofs,
	String sCar2, String sCarI, String sMesh, String sEnt,
	bool ghost, uint32 visFlags,
	AxisAlignedBox* bbox, String stMtr, VERTEXARRAY* var, bool bLogInfo)
{
	if (FileExists(sCar2 + sMesh))
	{
		Entity* ent = mSceneMgr->createEntity(sCarI + sEnt, sDirname + sMesh, sCarI);
		if (bbox)  *bbox = ent->getBoundingBox();
		if (ghost)  {  ent->setRenderQueueGroup(RQG_CarGhost);  ent->setCastShadows(false);  }
		else  if (visFlags == RV_CarGlass)  ent->setRenderQueueGroup(RQG_CarGlass);
		ndCar->attachObject(ent);  ent->setVisibilityFlags(visFlags);
		if (bLogInfo)  LogMeshInfo(ent, sDirname + sMesh);
	}
	else
	{	ManualObject* mo = pApp->CreateModel(mSceneMgr, stMtr, var, vPofs, false, false, sCarI+sEnt);
		if (!mo)  return;
		if (bbox)  *bbox = mo->getBoundingBox();
		if (ghost)  {  mo->setRenderQueueGroup(RQG_CarGhost);  mo->setCastShadows(false);  }
		else  if (visFlags == RV_CarGlass)  mo->setRenderQueueGroup(RQG_CarGlass);
		ndCar->attachObject(mo);  mo->setVisibilityFlags(visFlags);
	
		/** ///  save .mesh
		MeshPtr mpCar = mInter->convertToMesh("Mesh" + sEnt);
		MeshSerializer* msr = new MeshSerializer();
		msr->exportMesh(mpCar.getPointer(), sDirname + sMesh);/**/
	}
}


//-------------------------------------------------------------------------------------------------------
//  Create
//-------------------------------------------------------------------------------------------------------
void CarModel::Create(int car)
{
	if (!pCar)  return;

	String strI = toStr(iIndex)+ (eType == CT_TRACK ? "Z" : (eType == CT_GHOST2 ? "V" :""));
	mtrId = strI;
	String sCarI = "Car" + strI;
	resGrpId = sCarI;

	String sCars = PATHMANAGER::Cars() + "/" + sDirname;
	resCar = sCars + "/textures";
	String rCar = resCar + "/" + sDirname;
	String sCar = sCars + "/" + sDirname;
	
	bool ghost = false;  //isGhost();  //1 || for ghost test
	bool bLogInfo = !isGhost();  // log mesh info
	bool ghostTrk = isGhostTrk();
	
	//  Resource locations -----------------------------------------
	/// Add a resource group for this car
	ResourceGroupManager::getSingleton().createResourceGroup(resGrpId);
	Ogre::Root::getSingletonPtr()->addResourceLocation(sCars, "FileSystem", resGrpId);
	Ogre::Root::getSingletonPtr()->addResourceLocation(sCars + "/textures", "FileSystem", resGrpId);
		
	pMainNode = mSceneMgr->getRootSceneNode()->createChildSceneNode();
	SceneNode* ndCar = pMainNode->createChildSceneNode();

	//  --------  Follow Camera  --------
	if (mCamera)
	{
		fCam = new FollowCamera(mCamera, pSet);
		fCam->chassis = pCar->dynamics.chassis;
		fCam->loadCameras();
		
		//  set in-car camera position to driver position
		for (std::vector<CameraAngle*>::iterator it=fCam->mCameraAngles.begin();
			it!=fCam->mCameraAngles.end(); ++it)
		{
			if ((*it)->mName == "Car driver")
				(*it)->mOffset = Vector3(driver_view[0], driver_view[2], -driver_view[1]);
			else if ((*it)->mName == "Car bonnet")
				(*it)->mOffset = Vector3(hood_view[0], hood_view[2], -hood_view[1]);
		}
	}
			
	CreateReflection();
	

	//  next checkpoint marker
	bool deny = pApp->gui->pChall && !pApp->gui->pChall->chk_beam;
	if (eType == CT_LOCAL && !deny)
	{
		entNextChk = mSceneMgr->createEntity("Chk"+strI, "check.mesh");
		entNextChk->setRenderQueueGroup(RQG_Weather);  entNextChk->setCastShadows(false);
		ndNextChk = mSceneMgr->getRootSceneNode()->createChildSceneNode();
		ndNextChk->attachObject(entNextChk);  entNextChk->setVisibilityFlags(RV_Hud);
		ndNextChk->setVisible(false);
	}


	///()  grass sphere test
	#if 0
	Entity* es = mSceneMgr->createEntity(sCarI+"s", "sphere.mesh", sCarI);
	es->setRenderQueueGroup(RQG_CarGhost);
	MaterialPtr mtr = MaterialManager::getSingleton().getByName("pipeGlass");
	es->setMaterial(mtr);
	ndSph = mSceneMgr->getRootSceneNode()->createChildSceneNode();
	ndSph->attachObject(es);
	#endif


	///  Create Models:  body, interior, glass
	//-------------------------------------------------
	Vector3 vPofs(0,0,0);
	AxisAlignedBox bodyBox;  uint8 g = RQG_CarGhost;
	all_subs=0;  all_tris=0;  //stats
	
	if (bRotFix)
		ndCar->setOrientation(Quaternion(Degree(90),Vector3::UNIT_Y)*Quaternion(Degree(180),Vector3::UNIT_X));


	CreatePart(ndCar, vPofs, sCar, sCarI, "_body.mesh",     "",  ghost, RV_Car,  &bodyBox,  sMtr[Mtr_CarBody], &pCar->bodymodel.mesh,     bLogInfo);

	vPofs = Vector3(interiorOffset[0],interiorOffset[1],interiorOffset[2]);  //x+ back y+ down z+ right
	if (!ghost)
	CreatePart(ndCar, vPofs, sCar, sCarI, "_interior.mesh", "i", ghost, RV_Car,      0, sMtr[Mtr_CarBody]+"i", &pCar->interiormodel.mesh, bLogInfo);

	vPofs = Vector3::ZERO;
	CreatePart(ndCar, vPofs, sCar, sCarI, "_glass.mesh",    "g", ghost, RV_CarGlass, 0, sMtr[Mtr_CarBody]+"g", &pCar->glassmodel.mesh,    bLogInfo);
	

	//  wheels  ----------------------
	for (int w=0; w < 4; ++w)
	{
		String siw = "Wheel" + strI + "_" + toStr(w);
		ndWh[w] = mSceneMgr->getRootSceneNode()->createChildSceneNode();

		String sMesh = "_wheel.mesh";  // custom
		if (w <  2  && FileExists(sCar + "_wheel_front.mesh"))  sMesh = "_wheel_front.mesh"; else  // 2|
		if (w >= 2  && FileExists(sCar + "_wheel_rear.mesh") )  sMesh = "_wheel_rear.mesh";  else
		if (w%2 == 0 && FileExists(sCar + "_wheel_left.mesh") )  sMesh = "_wheel_left.mesh";  else  // 2-
		if (w%2 == 1 && FileExists(sCar + "_wheel_right.mesh"))  sMesh = "_wheel_right.mesh"; /*else
		if (w == 0  && FileExists(sCar + "_wheel_fl.mesh"))  sMesh = "_wheel_fl.mesh"; else  // all 4
		if (w == 1  && FileExists(sCar + "_wheel_fr.mesh"))  sMesh = "_wheel_fr.mesh"; else
		if (w == 2  && FileExists(sCar + "_wheel_rl.mesh"))  sMesh = "_wheel_rl.mesh"; else
		if (w == 3  && FileExists(sCar + "_wheel_rr.mesh"))  sMesh = "_wheel_rr.mesh"; /**/
		
		if (FileExists(sCar + sMesh))
		{
			String name = sDirname + sMesh;
			Entity* eWh = mSceneMgr->createEntity(siw, sDirname + sMesh, sCarI);
			if (ghost)  {  eWh->setRenderQueueGroup(g);  eWh->setCastShadows(false);  }
			ndWh[w]->attachObject(eWh);  eWh->setVisibilityFlags(RV_Car);
			if (bLogInfo && w==0)  LogMeshInfo(eWh, name, 4);
		}else
		{	ManualObject* mWh = pApp->CreateModel(mSceneMgr, sMtr[Mtr_CarBody]+siw, &pCar->wheelmodelfront.mesh, vPofs, true, false, siw);
			if (mWh)  {
			if (ghost)  {  mWh->setRenderQueueGroup(g);  mWh->setCastShadows(false);  }
			ndWh[w]->attachObject(mWh);  mWh->setVisibilityFlags(RV_Car);  }
		}
		
		if (FileExists(sCar + "_brake.mesh") && !ghostTrk)
		{
			String name = sDirname + "_brake.mesh";
			Entity* eBrake = mSceneMgr->createEntity(siw + "_brake", name, sCarI);
			if (ghost)  {  eBrake->setRenderQueueGroup(g);  eBrake->setCastShadows(false);  }
			ndBrake[w] = ndWh[w]->createChildSceneNode();
			ndBrake[w]->attachObject(eBrake);  eBrake->setVisibilityFlags(RV_Car);
			if (bLogInfo && w==0)  LogMeshInfo(eBrake, name, 4);
		}
	}
	if (bLogInfo)  // all
		LogO("MESH info:  "+sDirname+"\t ALL sub: "+toStr(all_subs)+"  tri: "+fToStr(all_tris/1000.f,1,4)+"k");
	
	
	///  brake flares  ++ ++
	if (!brakePos.empty())
	{
		SceneNode* nd = ndCar->createChildSceneNode();
		brakes = mSceneMgr->createBillboardSet("Flr"+strI,2);
		brakes->setDefaultDimensions(brakeSize, brakeSize);
		brakes->setRenderQueueGroup(RQG_CarTrails);  //brakes->setVisibilityFlags();

		for (int i=0; i < brakePos.size(); ++i)
			brakes->createBillboard(brakePos[i], brakeClr);

		brakes->setVisible(false);
		brakes->setMaterialName("flare1");
		nd->attachObject(brakes);
	}
	
	if (!ghostTrk)
	{
		//  Particles
		//-------------------------------------------------
		///  world hit sparks
		if (!parHit)  {
			parHit = mSceneMgr->createParticleSystem("Hit" + strI, "Sparks");
			parHit->setVisibilityFlags(RV_Particles);
			mSceneMgr->getRootSceneNode()->createChildSceneNode()->attachObject(parHit);
			parHit->getEmitter(0)->setEmissionRate(0);  }

		///  boost emitters  ------------------------
		for (int i=0; i < 2; i++)
		{
			String si = strI + "_" +toStr(i);
			if (!parBoost[i])  {
				parBoost[i] = mSceneMgr->createParticleSystem("Boost"+si, sBoostParName);
				parBoost[i]->setVisibilityFlags(RV_Particles);
				if (!pSet->boostFromExhaust || !manualExhaustPos)
				{
					// no exhaust pos in car file, guess from bounding box
					Vector3 bsize = (bodyBox.getMaximum() - bodyBox.getMinimum())*0.5,
						bcenter = bodyBox.getMaximum() + bodyBox.getMinimum();
					//LogO("Car body bbox :  size " + toStr(bsize) + ",  center " + toStr(bcenter));
					Vector3 vp = bRotFix ?
						Vector3(bsize.z * 0.97, bsize.y * 0.65, bsize.x * 0.65 * (i==0 ? 1 : -1)) :
						Vector3(bsize.x * 0.97, bsize.y * 0.65, bsize.z * 0.65 * (i==0 ? 1 : -1));
						//Vector3(1.9 /*back*/, 0.1 /*up*/, 0.6 * (i==0 ? 1 : -1)/*sides*/
					vp.z *= boostSizeZ;
					vp += Vector3(boostOffset[0],boostOffset[1],boostOffset[2]);
					SceneNode* nb = pMainNode->createChildSceneNode(bcenter+vp);
					nb->attachObject(parBoost[i]);
				}else{
					// use exhaust pos values from car file
					Vector3 pos;
					if (i==0)
						pos = Vector3(exhaustPos[0], exhaustPos[1], exhaustPos[2]);
					else if (!has2exhausts)
						continue;
					else
						pos = Vector3(exhaustPos[0], exhaustPos[1], -exhaustPos[2]);

					SceneNode* nb = pMainNode->createChildSceneNode(pos);
					nb->attachObject(parBoost[i]); 
				}
				parBoost[i]->getEmitter(0)->setEmissionRate(0);
			}
		}

		///  wheel emitters  ------------------------
		if (!ghost)
		{
			const static String sPar[PAR_ALL] = {"Smoke","Mud","Dust","FlWater","FlMud","FlMudS"};  // for ogre name
			//  particle type names
			const String sName[PAR_ALL] = {sc->sParSmoke, sc->sParMud, sc->sParDust, "FluidWater", "FluidMud", "FluidMudSoft"};
			for (int w=0; w < 4; ++w)
			{
				String siw = strI + "_" +toStr(w);
				//  particles
				for (int p=0; p < PAR_ALL; ++p)
				if (!par[p][w])
				{
					par[p][w] = mSceneMgr->createParticleSystem(sPar[p]+siw, sName[p]);
					par[p][w]->setVisibilityFlags(RV_Particles);
					mSceneMgr->getRootSceneNode()->createChildSceneNode()->attachObject(par[p][w]);
					par[p][w]->getEmitter(0)->setEmissionRate(0.f);
				}
				//  trails
				if (!ndWhE[w])
					ndWhE[w] = mSceneMgr->getRootSceneNode()->createChildSceneNode();

				if (!whTrail[w])
				{	NameValuePairList params;
					params["numberOfChains"] = "1";
					params["maxElements"] = toStr(320 * pSet->trails_len);

					whTrail[w] = (RibbonTrail*)mSceneMgr->createMovableObject("RibbonTrail", &params);
					whTrail[w]->setInitialColour(0, 0.1,0.1,0.1, 0);
					whTrail[w]->setFaceCamera(false,Vector3::UNIT_Y);
					mSceneMgr->getRootSceneNode()->attachObject(whTrail[w]);
					whTrail[w]->setMaterialName("TireTrail");
					whTrail[w]->setCastShadows(false);
					whTrail[w]->addNode(ndWhE[w]);
				}
				whTrail[w]->setTrailLength(90 * pSet->trails_len);  //30
				whTrail[w]->setInitialColour(0, 0.1f,0.1f,0.1f, 0);
				whTrail[w]->setColourChange(0, 0.0,0.0,0.0, /*fade*/0.08f * 1.f / pSet->trails_len);
				whTrail[w]->setInitialWidth(0, 0.f);
			}
		}
		UpdParsTrails();
	}

	RecreateMaterials();
		
	setMtrNames();
	
	//  this snippet makes sure the brake texture is pre-loaded.
	//  since it is not used until you actually brake, we have to explicitely declare it
	ResourceGroupManager& resMgr = ResourceGroupManager::getSingleton();
	if (FileExists(rCar + "_body00_brake.png")) resMgr.declareResource(sDirname + "_body00_brake.png", "Texture", resGrpId);
	if (FileExists(rCar + "_body00_add.png"))   resMgr.declareResource(sDirname + "_body00_add.png", "Texture", resGrpId);
	
	//  now just preload the whole resource group
	resMgr.initialiseResourceGroup(resGrpId);
	resMgr.loadResourceGroup(resGrpId);
}


//-------------------------------------------------------------------------------------------------------
//  materials
//-------------------------------------------------------------------------------------------------------
void CarModel::RecreateMaterials()
{
	String sCar = resCar + "/" + sDirname;
	bool ghost = false;  //isGhost();  //1 || for ghost test
	
	// --------- Materials  -------------------
	
	// if specialised car material (e.g. car_body_FM) exists, use this one instead of e.g. car_body
	// useful macro for choosing between these 2 variants
	#define chooseMat(s)  MaterialManager::getSingleton().resourceExists("car"+String(s) + "_"+sDirname) ? "car"+String(s) + "_"+sDirname : "car"+String(s)

	//  ghost car has no interior, particles, trails and uses same material for all meshes
	if (!ghost)
	{	sMtr[Mtr_CarBody]     = chooseMat("_body");
		sMtr[Mtr_CarBrake]    = chooseMat("_glass");
	}else
	for (int i=0; i < NumMaterials; ++i)
		sMtr[i] = "car_ghost";

	//  copy material to a new material with index
	MaterialPtr mat;
	for (int i=0; i < 1/*NumMaterials*/; ++i)
	{
		sh::Factory::getInstance().destroyMaterialInstance(sMtr[i] + mtrId);
		sh::MaterialInstance* m = sh::Factory::getInstance().createMaterialInstance(sMtr[i] + mtrId, sMtr[i]);

		m->setListener(this);

		// change textures for the car
		if (m->hasProperty("diffuseMap"))
		{
			std::string v = sh::retrieveValue<sh::StringValue>(m->getProperty("diffuseMap"), 0).get();
			m->setProperty("diffuseMap", sh::makeProperty<sh::StringValue>(new sh::StringValue(sDirname + "_" + v)));
		}
		if (m->hasProperty("carPaintMap"))
		{
			std::string v = sh::retrieveValue<sh::StringValue>(m->getProperty("carPaintMap"), 0).get();
			m->setProperty("carPaintMap", sh::makeProperty<sh::StringValue>(new sh::StringValue(sDirname + "_" + v)));
		}
		if (m->hasProperty("reflMap"))
		{
			std::string v = sh::retrieveValue<sh::StringValue>(m->getProperty("reflMap"), 0).get();
			m->setProperty("reflMap", sh::makeProperty<sh::StringValue>(new sh::StringValue(sDirname + "_" + v)));
		}
		sMtr[i] = sMtr[i] + mtrId;
	}

	//ChangeClr();

	UpdateLightMap();
}

void CarModel::setMtrName(const String& entName, const String& mtrName)
{
	if (mSceneMgr->hasEntity(entName))
		mSceneMgr->getEntity(entName)->setMaterialName(mtrName);
	else
	if (mSceneMgr->hasManualObject(entName))
		mSceneMgr->getManualObject(entName)->setMaterialName(0, mtrName);
}

void CarModel::setMtrNames()
{
	//if (FileExists(resCar + "/" + sDirname + "_body00_add.png") ||
	//	FileExists(resCar + "/" + sDirname + "_body00_red.png"))
	setMtrName("Car"+mtrId, sMtr[Mtr_CarBody]);

	#if 0
	setMtrName("Car.interior"+mtrI, sMtr[Mtr_CarInterior]);
	setMtrName("Car.glass"+mtrI, sMtr[Mtr_CarGlass]);

	for (int w=0; w < 4; ++w)
	{
		String sw = "Wheel"+mtrI+"_"+toStr(w), sm = w < 2 ? sMtr[Mtr_CarTireFront] : sMtr[Mtr_CarTireRear];
		setMtrName(sw,          sm);
		setMtrName(sw+"_brake", sm);
	}
	#endif
}

//  ----------------- Reflection ------------------------
void CarModel::CreateReflection()
{
	pReflect = new CarReflection(pSet, pApp, mSceneMgr, iIndex);
	for (int i=0; i < NumMaterials; i++)
		pReflect->sMtr[i] = sMtr[i];

	pReflect->Create();
}

void CarModel::requestedConfiguration(sh::MaterialInstance* m, const std::string& configuration)
{
}

void CarModel::createdConfiguration(sh::MaterialInstance* m, const std::string& configuration)
{
	UpdateLightMap();
	ChangeClr();
}

#include "Client.h"
#include "Message.h"
#include "Unit.h"
#include <thread>
#include <chrono>
using namespace std::literals;

std::unique_ptr<Client> localClient;

bool Client::resolveAutoBinding(const char * autoBinding, Node * node, MaterialParameter * parameter) {
#define CMP(x) (strcmp(autoBinding,x)==0)
    if (CMP("SHADOW_MAP"))
        parameter->setSampler(mShadowMap.get());
    else if (CMP("MAP_SIZE"))
        parameter->setInt(shadowSize);
    else if (CMP("LIGHT_MATRIX"))
        parameter->bindValue(this, &Client::getMat);
    else if (CMP("BIAS"))
        parameter->setValue(bias);
    else return false;
#undef CMP
    return true;
}

Matrix Client::getMat() const {
    return mLightSpace;
}

void Client::drawNode(Node * node, const char* effect) {
    if (node->isEnabled() && node->getDrawable()) {
        auto bs = node->getBoundingSphere();
        if (*effect == 'w')bs.center.y = -bs.center.y;
        if (bs.intersects(mScene->getActiveCamera()->getFrustum())) {
            auto m = dynamic_cast<Model*>(node->getDrawable());
            auto t = dynamic_cast<Terrain*>(node->getDrawable());

            if (m) m->getMaterial()->setTechnique(effect);
            else if (t) {
                for (unsigned int i = 0; i < t->getPatchCount(); ++i)
                    t->getPatch(i)->getMaterial(0)->setTechnique(effect);
            }

            if (effect[0] == 's' || m || t)
                node->getDrawable()->draw();
        }
    }

    for (auto i = node->getFirstChild(); i; i = i->getNextSibling())
        drawNode(i, effect);
}

Vector3 Client::getPoint(int x, int y) const {
    auto game = Game::getInstance();
    Ray ray;
    auto rect = gameplay::Rectangle(game->getWidth() - mRight, game->getHeight());
    mCamera->setAspectRatio(rect.width / rect.height);
    mCamera->pickRay(rect, x, y, &ray);
    Vector3 maxwell;
    float minError = std::numeric_limits<float>::max();
    auto reslover = [ray](float x) {return ray.getOrigin() -
        ray.getDirection()*(ray.getOrigin().y*x / ray.getDirection().y); };
    constexpr auto num = 16.0f;
    for (uint8_t i = 0; i < num; ++i) {
        auto x = i / num;
        auto p = reslover(x);
        auto error = std::abs(p.y - mMap->getHeight(p.x, p.z));
        if (error < minError)
            minError = error, maxwell = p;
    }
    return maxwell;
}

bool Client::checkCamera() {
    auto game = Game::getInstance();
    auto rect = gameplay::Rectangle(game->getWidth() - mRight, game->getHeight());
    mCamera->setAspectRatio(rect.width / rect.height);
    auto test = [this, rect](float x, float y) {
        Ray r;
        mCamera->pickRay(rect, x, y, &r);
        BoundingBox b(-mapSizeHF, 0.0f, -mapSizeHF, mapSizeHF, 1.0f, mapSizeHF);
        return b.intersects(r) == Ray::INTERSECTS_NONE;
    };
    return test(0, 0) || test(0, rect.height) || test(rect.width, 0) || test(rect.width, rect.height);
}

void Client::move(int x, int y) {
    if (mChoosed.size()) {
        RakNet::BitStream data;
        data.Write(ClientMessage::setMoveTarget);
        auto p = getPoint(x, y);
        data.Write(Vector2{ p.x,p.z });
        data.Write(static_cast<uint32_t>(mChoosed.size()));
        for (auto x : mChoosed)
            data.Write(x);
        mPeer->Send(&data, PacketPriority::HIGH_PRIORITY,
            PacketReliability::RELIABLE, 0, mServer, false);
    }
}

Client::Client(const std::string & server, bool& res) :
    mPeer(RakNet::RakPeerInterface::GetInstance()), mServer(server.c_str(), 23333),
    mState(false), mWeight(globalUnits.size(), 1), mSpeed(1.0f) {

    RakNet::SocketDescriptor SD;
    mPeer->Startup(1, &SD, 1);
    mPeer->SetMaximumIncomingConnections(1);
    auto result = mPeer->Connect(server.c_str(), 23333, nullptr, 0);
    auto wait = [&] {
        auto t = std::chrono::system_clock::now();
        while ((mPeer->NumberOfConnections() == 0) &&
            (std::chrono::system_clock::now() - t <= 500ms))
            std::this_thread::yield();
        return mPeer->NumberOfConnections();
    };
    
    if (result != RakNet::CONNECTION_ATTEMPT_STARTED
        || !wait())
        INFO("Failed to connect to the server.");

    res = mPeer->NumberOfConnections();
    if (!res)return;

    mRECT = SpriteBatch::create("res/common/rect.png");
    mMiniMapUnit = SpriteBatch::create("res/common/white.png");
    mHot = SpriteBatch::create("res/common/hot.png");

    mDepth = FrameBuffer::create("depth", shadowSize, shadowSize, Texture::Format::RGBA8888);

    uniqueRAII<DepthStencilTarget> shadow = DepthStencilTarget::create("shadow",
        DepthStencilTarget::DEPTH, shadowSize, shadowSize);
    mDepth->setDepthStencilTarget(shadow.get());

    uniqueRAII<Texture> texture = mDepth->getRenderTarget()->getTexture();
    mShadowMap = Texture::Sampler::create(texture.get());
    mShadowMap->setFilterMode(Texture::LINEAR, Texture::LINEAR);
    mShadowMap->setWrapMode(Texture::CLAMP, Texture::CLAMP);
    mLight = Node::create();
    
    uniqueRAII<Camera> light = Camera::createOrthographic(1.0f, 1.0f, 1.0f, 1.0f, 2.0f);
    mLight->setCamera(light.get());

    auto f = -Vector3::one();
    correctVector(mLight.get(), &Node::getForwardVector, f.normalize(), M_PI, M_PI, 0.0f);

    uniqueRAII<Scene> model = Scene::load("res/common/common.scene");
    mFlagModel = model->findNode("key")->clone();
    mWaterPlane = model->findNode("plane")->clone();
    mWaterPlane->scale(1.4f*mapSizeHF / mWaterPlane->getBoundingSphere().radius);

    if (reflection>0.0f) {
        auto old = FrameBuffer::getCurrent();
        mWaterBuffer = DepthStencilTarget::create("water",
            DepthStencilTarget::DEPTH_STENCIL,old->getWidth(),old->getHeight());
    }

    {
        mStateInfo = Form::create("label", Theme::getDefault()->getStyle("Label"));
        mStateInfo->setAutoSize(Control::AutoSize::AUTO_SIZE_BOTH);
        mStateInfo->setConsumeInputEvents(false);
        mStateInfo->setAlignment(Control::Alignment::ALIGN_BOTTOM_HCENTER);
        uniqueRAII<Label> info = Label::create("info");
        mStateInfo->addControl(info.get());
        info->setAutoSize(Control::AutoSize::AUTO_SIZE_BOTH);
        info->setAlignment(Control::Alignment::ALIGN_BOTTOM_HCENTER);
        info->setConsumeInputEvents(false);
    }
}

Client::~Client() {
    mPeer->Shutdown(500, 0, PacketPriority::IMMEDIATE_PRIORITY);
    RakNet::RakPeerInterface::DestroyInstance(mPeer);
}

void Client::changeGroup(uint8_t group) {
    mGroup = group;
    RakNet::BitStream stream;
    stream.Write(ClientMessage::changeGroup);
    stream.Write(group);
    mPeer->Send(&stream, PacketPriority::IMMEDIATE_PRIORITY, PacketReliability::RELIABLE_ORDERED
        , 0, mServer, false);
}

Client::WaitResult Client::wait() {
    auto packet = mPeer->Receive();
    if (packet) {
        RakNet::BitStream data(packet->data, packet->length, false);
        data.IgnoreBytes(1);
        CheckBegin;
        CheckHeader(ID_DISCONNECTION_NOTIFICATION) {
            mPeer->DeallocatePacket(packet);
            return WaitResult::Disconnected;
        }
        CheckHeader(ServerMessage::info) {
            uint64_t key;
            data.Read(key);
            if (key != pakKey) {
                INFO("The pakKey of server is not equal yours.");
                mPeer->DeallocatePacket(packet);
                return WaitResult::Disconnected;
            }
            RakNet::RakString str;
            data.Read(str);
            INFO("Loading map ", str.C_String());
            mMap = std::make_unique<Map>(str.C_String());
            mMiniMap = SpriteBatch::create(("res/maps/"s + str.C_String() + "/view.png").c_str());
            uniqueRAII<Scene> sky = Scene::load(("res/maps/"s + str.C_String() + "/sky.scene").c_str());
            mSky = sky->findNode("sky")->clone();
            mScene->addNode(mSky.get());
        }
        CheckHeader(ServerMessage::changeSpeed) {
            data.Read(mSpeed);
        }
        CheckHeader(ServerMessage::go) {
            mScene = Scene::create();
            mCamera =
                Camera::createPerspective(45.0f, Game::getInstance()->getAspectRatio(), 1.0f, 5000.0f);
            mScene->addNode()->setCamera(mCamera.get());
            mScene->setActiveCamera(mCamera.get());
            mScene->addNode(mFlagModel.get());
            mMap->set(mScene->addNode("terrain"));
            auto c = mCamera->getNode();
            c->rotateX(-M_PI_2);
            Vector2 p;
            data.Read(p);
            mCameraPos = { p.x, mMap->getHeight(p.x, p.y) + 200.0f, p.y };
            c->setTranslation(mCameraPos);
            mScene->addNode(mLight.get());
            mX = mY = mBX = mBY = 0;
            std::fill(mWeight.begin(), mWeight.end(), 1);
            mScene->addNode(mSky.get());
            mScene->addNode(mWaterPlane.get());
            mState = true;

            mPeer->DeallocatePacket(packet);
            return WaitResult::Go;
        }
        mPeer->DeallocatePacket(packet);
        return wait();
    }
    return WaitResult::None;
}

void Client::stop() {
    mState = false;
    mScene.reset();
    mCamera.reset();
    mUnits.clear();
    mDuang.clear();
    mChoosed.clear();
    mHotPoint.clear();
    RakNet::BitStream stream;
    stream.Write(ClientMessage::exit);
    mPeer->Send(&stream, PacketPriority::IMMEDIATE_PRIORITY, PacketReliability::RELIABLE_ORDERED
        , 0, mServer, false);
}

bool Client::update(float delta) {

    if (checkCamera())
        mCamera->getNode()->setTranslation(mCameraPos);
    else
        mCameraPos = mCamera->getNode()->getTranslation();

#ifdef WIN32
    if (!(mBX || mBY)) {
        auto game = Game::getInstance();
        auto rect = gameplay::Rectangle(game->getWidth() - mRight, game->getHeight());
        float vx = (mX - rect.width / 2) / rect.width;
        float vy = (mY - rect.height / 2) / rect.height;
        constexpr float unit = 0.001f;
        float m = unit*delta;
        float mx = (std::abs(vx) <= 0.5 && std::abs(vx) >= 0.4) ? (vx > 0.0f ? m : -m) : 0.0f;
        float my = (std::abs(vy) <= 0.5 && std::abs(vy) >= 0.4) ? (vy > 0.0f ? m : -m) : 0.0f;
        moveEvent(mx, 0.0f);
        moveEvent(0.0f, my);
    }
#endif // WIN32

    delta *= mSpeed;

    auto isStop = false;
    for (auto packet = mPeer->Receive(); packet; mPeer->DeallocatePacket(packet), packet = mPeer->Receive()) {
        if (isStop)continue;
        RakNet::BitStream data(packet->data, packet->length, false);
        data.IgnoreBytes(1);
        CheckBegin;
        CheckHeader(ServerMessage::stop) {
            isStop = true;
            continue;
        }
        CheckHeader(ServerMessage::win) {
            INFO("We are winner!");
        }
        CheckHeader(ServerMessage::out) {
            INFO("What a pity!");
            isStop = true;
            continue;
        }
        CheckHeader(ServerMessage::updateUnit) {
            uint32_t size;
            data.Read(size);
            std::set<uint32_t> old;
            for (auto&& u : mUnits)
                old.insert(u.first);
            for (uint32_t i = 0; i < size; ++i) {
                UnitSyncInfo u;
                data.Read(u);
                auto oi = old.find(u.id);
                if (oi != old.cend()) {
                    mUnits[u.id].getNode()->setTranslation(u.pos);
                    mUnits[u.id].getNode()->setRotation(u.rotation);
                    mUnits[u.id].setAttackTarget(u.at);
                    old.erase(oi);
                }
                else {
                    auto i = mUnits.insert({ u.id,
                        std::move(UnitInstance{ getUnit(u.kind), u.group, u.id, mScene.get(), false, u.pos }) });
                    i.first->second.getNode()->setRotation(u.rotation);
                    i.first->second.update(0);
                    i.first->second.setAttackTarget(u.at);
                }
                if (u.isDied)
                    mUnits[u.id].attacked(1e10f);
            }
            for (auto&& o : old) {
                mScene->removeNode(mUnits[o].getNode());
                mUnits.erase(o);
            }
        }
        CheckHeader(ServerMessage::updateBullet) {
            uint32_t size;
            data.Read(size);
            std::set<uint32_t> old;
            for (auto&& x : mBullets)
                old.insert(x.first);
            for (uint32_t i = 0; i < size; ++i) {
                BulletSyncInfo info;
                data.Read(info);
                auto iter = old.find(info.id);
                if (iter == old.cend()) {
                    mBullets.insert({ info.id,std::move(BulletInstance(info.kind, {}, {}, 0.0f, 0.0f, 0.0f,0)) });
                    mScene->addNode(mBullets[info.id].getNode());
                }
                else old.erase(iter);
                mBullets[info.id].getNode()->setTranslation(info.pos);
                mBullets[info.id].getNode()->setRotation(info.rotation);
            }

            for (auto&& x : old) {
                mScene->removeNode(mBullets[x].getNode());
                mBullets.erase(x);
            }
        }
        CheckHeader(ServerMessage::updateWeight) {
            for (auto& x : mWeight)
                data.Read(x);
        }
        CheckHeader(ServerMessage::duang) {
            if (!enableParticle)continue;
            uint16_t size;
            data.Read(size);
            float now = Game::getAbsoluteTime();
            for (uint16_t i = 0; i < size; ++i) {
                DuangSyncInfo info;
                data.Read(info);
                auto& bullet = getBullet(info.kind);
                auto iter = mDuang.insert({ bullet.boom(),now + bullet.getBoomTime() }).first;
                mScene->addNode(iter->emitter.get());
                iter->emitter->setTranslation(info.pos);
                auto p = dynamic_cast<ParticleEmitter*>(iter->emitter->getDrawable());
                p->start();
                mHotPoint.push_back({ info.pos.x,info.pos.z });
                if (mHotPoint.size() > 4)
                    mHotPoint.pop_front();
            }
        }
    }

    if (isStop || mPeer->GetConnectionState(mServer) != RakNet::IS_CONNECTED) {
        INFO("The game stopped.");
        stop();
        return false;
    }

    for (auto&& x : mUnits)
        x.second.update(delta);

    if (enableParticle) {
        std::vector<decltype(mDuang)::const_iterator> deferred;
        auto end = Game::getAbsoluteTime();
        for (auto i = mDuang.cbegin(); i != mDuang.cend(); ++i)
            if (i->end < end)
                deferred.emplace_back(i);
            else
                dynamic_cast<ParticleEmitter*>(i->emitter->getDrawable())->update(delta);
        for (auto&& x : deferred) {
            mScene->removeNode(x->emitter.get());
            mDuang.erase(x);
        }

        for (auto&& x : mBullets)
            x.second.updateClient(delta);
    }

    std::set<uint32_t> choosed;
    for (auto&& x : mChoosed)
        if (mUnits.find(x) != mUnits.cend())
            choosed.insert(x);
    choosed.swap(mChoosed);

    auto label = dynamic_cast<Label*>(mStateInfo->getControl(0U));
    if (mChoosed.size())label->setText(("Choosed "+to_string(mChoosed.size())).c_str());
    else label->setText("");
    mStateInfo->update(delta);

    return true;
}

void Client::render() {
    if (mState) {
        auto game = Game::getInstance();
        auto rect = gameplay::Rectangle(game->getWidth() - mRight, game->getHeight());

        if (shadowSize > 1) {
            static const char* depth = "depth";
            mDepth->bind();
            game->setViewport(gameplay::Rectangle(shadowSize, shadowSize));
            game->clear(Game::CLEAR_COLOR_DEPTH, Vector4::one(), 1.0f, 1.0f);
            auto y = mCamera->getNode()->getTranslationY();
            Matrix projection;
            auto p = getPoint(rect.width / 2, rect.height / 2);
            auto fn = (Vector3::one()*100.0f).length();
            Matrix::createOrthographic(y*2.0f, y*2.0f, 0.0f, fn*(y / 500.0f + 6.0f), &projection);
            mLight->setTranslation(p + Vector3::one()*100.0f);
            mLight->getCamera()->setProjectionMatrix(projection);
            mLightSpace = mLight->getCamera()->getViewProjectionMatrix();
            mScene->setActiveCamera(mLight->getCamera());
            for (auto&& x : mUnits)
                drawNode(x.second.getNode(), depth);
            for (auto&& x : mBullets)
                drawNode(x.second.getNode(), depth);

            for (auto&& p : mMap->getKey()) {
                mFlagModel->setTranslation(p.x, mMap->getHeight(p.x, p.y), p.y);
                drawNode(mFlagModel.get(), depth);
            }

            drawNode(mScene->findNode("terrain"), depth);

            mScene->setActiveCamera(mCamera.get());
            FrameBuffer::bindDefault();
        }

        game->setViewport(rect);
        mCamera->setAspectRatio(rect.width / rect.height);

        mScene->setAmbientColor(-0.8f, -0.8f, -0.8f);
        for (auto&& x : mUnits)
            if (x.second.isDied())
                drawNode(x.second.getNode());

        mScene->setAmbientColor(0.3f, 0.0f, 0.0f);
        for (auto&& x : mUnits)
            if (!x.second.isDied() && mChoosed.find(x.first) == mChoosed.cend()
                && x.second.getGroup() == mGroup)
                drawNode(x.second.getNode());

        std::vector<Node*> list;
        for (auto&& x : mUnits)
            if (!x.second.isDied() && mChoosed.find(x.first) != mChoosed.cend())
                list.emplace_back(x.second.getNode());

        game->clear(Game::CLEAR_STENCIL, {}, 0.0f, 0);

        for (auto&& x : list)
            drawNode(x, "choosedShadow");

        for (auto&& x : list) {
            auto s = x->getScale();
            x->scale(1.2f);
            drawNode(x, "choosed");
            x->setScale(s);
        }

        mScene->setAmbientColor(0.0f, 0.0f, 0.3f);
        for (auto&& x : mUnits)
            if (!x.second.isDied() && x.second.getGroup() != mGroup)
                drawNode(x.second.getNode());

        mScene->setAmbientColor(0.0f, 0.0f, 0.0f);
       
        for (auto&& p : mMap->getKey()) {
            mFlagModel->setTranslation(p.x, mMap->getHeight(p.x, p.y), p.y);
            drawNode(mFlagModel.get());
        }

        drawNode(mScene->findNode("terrain"));

        drawNode(mSky.get());

        uniqueRAII<DepthStencilTarget> old;
        if (reflection>0.0f) {
            old = FrameBuffer::getCurrent()->getDepthStencilTarget();
            FrameBuffer::getCurrent()->setDepthStencilTarget(mWaterBuffer.get());
            game->clear(Game::CLEAR_STENCIL, {}, 0.0f, 0);
        }

        glBlendColor(1.0f, 1.0f, 1.0f, waterAlpha);
        drawNode(mWaterPlane.get());

        if (reflection>0.0f) {
            glBlendColor(1.0f, 1.0f, 1.0f, reflection);
            auto cn = mCamera->getNode();

            game->clear(Game::CLEAR_DEPTH, {}, 1.0f, 0);

            static const char* water = "water";

            mScene->setAmbientColor(-0.8f, -0.8f, -0.8f);
            for (auto&& x : mUnits)
                if (x.second.isDied())
                    drawNode(x.second.getNode(), water);

            mScene->setAmbientColor(0.3f, 0.0f, 0.0f);
            for (auto&& x : mUnits)
                if (!x.second.isDied() && x.second.getGroup() == mGroup)
                    drawNode(x.second.getNode(), water);

            mScene->setAmbientColor(0.0f, 0.0f, 0.3f);
            for (auto&& x : mUnits)
                if (!x.second.isDied() && x.second.getGroup() != mGroup)
                    drawNode(x.second.getNode());

            mScene->setAmbientColor(0.0f, 0.0f, 0.0f);

            for (auto&& x : mBullets)
                drawNode(x.second.getNode(), water);

            drawNode(mScene->findNode("terrain"), water);

            drawNode(mSky.get(), water);

            FrameBuffer::getCurrent()->setDepthStencilTarget(old.get());
        }

        for (auto&& x : mBullets)
            drawNode(x.second.getNode());

        if (enableParticle)
            for (auto&& x : mDuang)
                drawNode(x.emitter.get());

        auto rect2 = gameplay::Rectangle(game->getWidth(), game->getHeight());
        game->setViewport(rect2);

        if (mBX&&mBY) {
            float x1 = mX, y1 = mY, x2 = mBX, y2 = mBY;
            if (x1 > x2)std::swap(x1, x2);
            if (y1 > y2)std::swap(y1, y2);
            gameplay::Rectangle range{ x1,y1,x2 - x1,y2 - y1 };
            mRECT->start();
            mRECT->draw(range, { 32,32 });
            mRECT->finish();
        }

        if (miniMapSize) {

            {
                gameplay::Rectangle range{ rect.width - miniMapSize,0.0f,
                    miniMapSize*1.0f,miniMapSize*1.0f };
                mMiniMap->start();
                auto texture = mMiniMap->getSampler()->getTexture();
                mMiniMap->draw(range, { texture->getWidth()*1.0f,texture->getHeight()*1.0f });
                mMiniMap->finish();
            }


            static const Vector4 red = { 1.0f,0.0f,0.0f,1.0f };
            static const Vector4 blue = { 0.0f,0.0f,1.0f,1.0f };

            auto fac = miniMapSize / mapSizeF;
            Vector2 base{ rect.width - miniMapSize / 2.0f,miniMapSize / 2.0f };

            if (mHotPoint.size()) {
                mHot->start();
                for (auto&& x : mHotPoint) {
                    auto dp = base + x*fac;
                    gameplay::Rectangle range{ dp.x - miniMapSize / 32.0f,dp.y - miniMapSize / 32.0f
                        ,miniMapSize / 16.0f,miniMapSize / 16.0f };
                    mHot->draw(range, { 32,32 });
                }
                mHot->finish();
            }

            mMiniMapUnit->start();
            for (auto&& p : mMap->getKey()) {
                auto dp = base + p*fac;
                gameplay::Rectangle range{ dp.x - miniMapSize / 64.0f,dp.y - miniMapSize / 64.0f
                    ,miniMapSize / 32.0f,miniMapSize / 32.0f };
                mMiniMapUnit->draw(range, { 1,1 });
            }

            for (auto&& x : mUnits)
                if (!x.second.isDied()) {
                    auto p = x.second.getRoughPos();
                    auto dp = base + Vector2(p.x, p.z)*fac;
                    gameplay::Rectangle range{ dp.x - miniMapSize / 128.0f,dp.y - miniMapSize / 128.0f
                        ,miniMapSize / 64.0f,miniMapSize / 64.0f };
                    mMiniMapUnit->draw(range, { 1,1 }, x.second.getGroup() == mGroup ? red : blue);
                }
            mMiniMapUnit->finish();

            auto p1 = getPoint(0, 0)*fac,
                p2 = getPoint(rect.width, rect.height)*fac;
            float x1 = p1.x, y1 = p1.z, x2 = p2.x, y2 = p2.z;
            if (x1 > x2)std::swap(x1, x2);
            if (y1 > y2)std::swap(y1, y2);
            gameplay::Rectangle range{ base.x + x1,base.y + y1,std::max(x2 - x1,16.0f),std::max(y2 - y1,16.0f) };
            mRECT->start();
            mRECT->draw(range, { 32,32 });
            mRECT->finish();
        }

        mStateInfo->draw();
    }
}

Vector3 Client::getPos(uint32_t id) {
    auto i = mUnits.find(id);
    if (i == mUnits.cend() || i->second.isDied())
        return Vector3::zero();
    return i->second.getNode()->getTranslation();
}

float Client::getHeight(int x, int z) const {
    return mMap->getHeight(x, z);
}

void Client::setViewport(uint32_t right) { mRight = right; }

void Client::changeWeight(const std::string& name, uint16_t weight) {
    uint16_t id = getUnitID(name);
    RakNet::BitStream data;
    data.Write(ClientMessage::changeWeight);
    data.Write(id);
    data.Write(weight);
    mPeer->Send(&data, PacketPriority::MEDIUM_PRIORITY, PacketReliability::RELIABLE, 0, mServer, false);
    mWeight[id] = weight;
}

uint16_t Client::getWeight(const std::string & name) const {
    return mWeight[getUnitID(name)];
}

uint16_t Client::getUnitNum() const {
    return mUnits.size();
}

void Client::moveEvent(float x, float y) {
    if (mState) {
        auto node = mCamera->getNode();
        auto pos = node->getTranslation();
        x *= pos.y, y *= pos.y;
        node->translate(x, 0.0f, y);
        auto height = mMap->getHeight(pos.x + x, pos.z + y);
        if (pos.y < height + 100.0f)
            node->setTranslationY(height + 100.0f);

        auto game = Game::getInstance();
        if (game->getWidth() && game->getHeight() && checkCamera())
            node->setTranslation(pos);
    }
}

void Client::scaleEvent(float x) {
    if (mState) {
        auto node = mCamera->getNode();
        auto pos = node->getTranslation();
        auto height = mMap->getHeight(pos.x, pos.z);
        if (pos.y + x > 10.0f && pos.y + x - 100.0f > height) {
            node->translateY(x);
            if (checkCamera())
                node->translateY(-x);
        }
    }
}

void Client::mousePos(int x, int y) {
    mX = x, mY = y;
}

void Client::beginPoint(int x, int y) {
    if (mState) {
        auto right = Game::getInstance()->getWidth()-mRight;
        if (y <= miniMapSize && x <= right && x >= right -miniMapSize ) {
            auto fac = mapSizeF / miniMapSize;
            Vector2 pos{ fac*(x - right + miniMapSize) - mapSizeHF,fac*y - mapSizeHF };
            if (mChoosed.empty()) {
                auto node = mCamera->getNode();
                auto old = node->getTranslation();
                node->setTranslation(pos.x, mMap->getHeight(pos.x, pos.y) +
                    std::max(100.0f, old.y - mMap->getHeight(old.x, old.z)), pos.y);
                while (checkCamera())
                    node->translateY(-50.0f);
            }
            else {
                RakNet::BitStream data;
                data.Write(ClientMessage::setMoveTarget);
                data.Write(pos);
                data.Write(static_cast<uint32_t>(mChoosed.size()));
                for (auto x : mChoosed)
                    data.Write(x);
                mPeer->Send(&data, PacketPriority::HIGH_PRIORITY,
                    PacketReliability::RELIABLE, 0, mServer, false);
            }
        }
        else mBX = x, mBY = y;
    }
}

void Client::endPoint(int x, int y) {
    auto toNDC = [](auto&& u, auto rect,auto offset) {
        auto MVP = u.second.getNode()->getWorldViewProjectionMatrix();
        auto NDC = MVP*offset;
        NDC.x /= NDC.w; NDC.y /= NDC.w; NDC.z /= NDC.w;
        NDC.x /= 2.0f, NDC.y /= 2.0f; NDC.z /= 2.0f;
        NDC.x += 0.5f; NDC.y += 0.5f; NDC.z += 0.5f;
        NDC.y = 1.0f - NDC.y;
        NDC.x *= rect.width;
        NDC.y *= rect.height;
        return NDC;
    };

    if (mState && mBX) {
        auto game = Game::getInstance();
        auto rect = gameplay::Rectangle(game->getWidth() - mRight, game->getHeight());
        mCamera->setAspectRatio(rect.width / rect.height);

        if ((mBX - x)*(mBX - x) + (mBY - y)*(mBY - y) > 256) {
            std::set<uint32_t> choosed, mine;
            if (mBX > x)std::swap(mBX, x);
            if (mBY > y)std::swap(mBY, y);
            for (auto&& u : mUnits) {
                auto NDC = toNDC(u, rect,Vector4::unitW());
                if (NDC.x >= mBX && NDC.x <= x && NDC.y >= mBY && NDC.y <= y) {
                    if (u.second.getGroup() != mGroup)choosed.insert(u.first);
                    else mine.insert(u.first);
                }
            }

            if (choosed.size()) {
                for (auto&& x : mine)
                    mChoosed.insert(x);

                RakNet::BitStream data;
                data.Write(ClientMessage::setAttackTarget);
                for (auto&& x : mChoosed) {
                    if (mUnits.find(x) == mUnits.cend())continue;
                    auto pos = mUnits[x].getRoughPos();
                    auto md = std::numeric_limits<float>::max();
                    uint32_t maxwell = 0;
                    for (auto&& y : choosed) {
                        auto p = mUnits[y].getRoughPos();
                        auto dis = p.distanceSquared(pos);
                        if (dis < md)md = dis, maxwell = y;
                    }
                    data.Write(x);
                    data.Write(maxwell);
                }
                mPeer->Send(&data, PacketPriority::HIGH_PRIORITY,
                    PacketReliability::RELIABLE_ORDERED, 0, mServer, false);
            }
            else mChoosed.swap(mine);
        }
        else {
            uint32_t choosed=0;
            float currectDepth = 1.0f;
            auto dis = [](auto x, auto y) {
                return x*x + y*y;
            };
            for (auto&& u : mUnits) {
                auto NDC = toNDC(u, rect,Vector4::unitW());
                auto right = toNDC(u, rect,  Vector4{
                    u.second.getKind().getRadius()/u.second.getNode()->getScaleX(),0.0f,0.0f,1.0f });
                auto radius =dis(right.x - NDC.x,right.y-NDC.y);
                if (NDC.z<currectDepth && dis(NDC.x-x,NDC.y-y)<=radius) {
                    currectDepth = NDC.z;
                    choosed = u.first;
                }
            }

            if (choosed) {
                if (mUnits[choosed].getGroup() == mGroup) {
                    mChoosed.clear();
                    mChoosed.insert(choosed);
                }
                else {
                    RakNet::BitStream data;
                    data.Write(ClientMessage::setAttackTarget);
                    for (auto&& x : mChoosed) {
                        if (mUnits.find(x) == mUnits.cend())continue;
                        data.Write(x);
                        data.Write(choosed);
                    }
                    mPeer->Send(&data, PacketPriority::HIGH_PRIORITY,
                        PacketReliability::RELIABLE_ORDERED, 0, mServer, false);
                }
            }
            else move(x, y);
        }
    }
    mBX = 0, mBY = 0;
}

void Client::cancel() {
    mChoosed.clear();
}

bool Client::isPlaying() const {
    return mState;
}

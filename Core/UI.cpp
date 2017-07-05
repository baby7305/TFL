#include "UI.h"
#include <sstream>
#include <typeinfo>
#include "Server.h"
#include "Client.h"
#include "Message.h"
#include "BuiltinAI.h"

std::stack<std::unique_ptr<UI>> UI::stack;
std::list<UI*> UI::parents;
std::list<std::unique_ptr<UI>> deferred;

void UI::addListener(Control* control) {
    int extEvent = PRESS;
    extEvent |= (typeid(*control) == typeid(TextBox)) ? TEXT_CHANGED : 0;
    extEvent |= (typeid(*control) == typeid(Slider)) ? VALUE_CHANGED : 0;
    control->addListener(this, extEvent);
    control->setConsumeInputEvents(dynamic_cast<Form*>(control) == nullptr);
    if (control->isContainer())
        for (auto child : dynamic_cast<Container*>(control)->getControls())
            addListener(child);
}
void UI::addListener() {
    addListener(mForm.get());
}
void UI::removeListener(Control * control) {
    control->removeListener(this);
    control->setConsumeInputEvents(false);
    if (control->isContainer())
        for (auto child : dynamic_cast<Container*>(control)->getControls())
            removeListener(child);
}

void UI::removeListener() {
    removeListener(mForm.get());
}

UI::UI(const std::string& name) :mForm(Form::create(("res/Forms.form#" + name).c_str())) {}

void UI::pop() {
    deferred.push_back(std::move(stack.top()));
    stack.pop();
}

static UI* current = nullptr;
void UI::updateForm(float delta) {
    if (current != stack.top().get()) {
        for (auto&& x : deferred)
            x->removeListener();
        deferred.clear();
        for (auto&& x : parents)
            x->removeListener();
        parents.clear();
        current = stack.top().get();
        current->addListener();
    }

    auto& form = stack.top()->mForm;
    auto align = form->getAlignment();
    form->setAlignment({});
    form->setAlignment(align);
    form->update(delta);

    stack.top()->update(delta);
}

void UI::render() {
    stack.top()->mForm->draw();
}

void UI::controlEvent(Control * control, EventType evt) {
    if(this==stack.top().get())
        event(control, evt);
}

void UI::update(float delta) {}

MainMenu::MainMenu() :UI("Main") {}

#define CMPID(id) (strcmp(id, control->getId()) == 0)
#define CHECKRET() if (evt == Event::PRESS && CMPID("return"))pop()

void MainMenu::event(Control* control, Event evt) {
    if (evt == Event::PRESS && CMPID("exit"))
        Game::getInstance()->exit();
    if (evt == Event::PRESS && CMPID("play"))
        push<PlayMenu>();
    if (evt == Event::PRESS && CMPID("about"))
        push<AboutMenu>();
    if (evt == Event::PRESS && CMPID("settings"))
        push<SettingsMenu>();
}

extern std::stringstream logtext;
AboutMenu::AboutMenu() :UI("About") {
    std::stringstream ss;
    ss << "Build at " __TIME__ " "  __DATE__ << std::endl
        << "By dtcxzyw and fish_head." << std::endl;
    get<Label>("info")->setText(ss.str().c_str());
    auto c = get<Container>("log");
    std::stringstream copy;
    copy << logtext.str();
    std::string s;
    while (std::getline(copy, s)) {
        auto l = Label::create("");
        l->setText(s.c_str());
        l->setTextColor({ 1.0f,0.0f,0.0f,1.0f });
        c->addControl(l);
    }
}

void AboutMenu::event(Control* control, Event evt) {
    CHECKRET();
    if (evt == Event::PRESS && CMPID("clear")) {
        std::stringstream ss;
        nativeSwap(ss, logtext);
        auto c = get<Container>("log");
        const_cast<std::vector<Control*>&>(c->getControls()).clear();
    }
    if (evt == Event::PRESS && CMPID("code"))
        Game::getInstance()->launchURL("https://github.com/dtcxzyw/TFL");
    if (evt == Event::PRESS && CMPID("report"))
        Game::getInstance()->launchURL("https://github.com/dtcxzyw/TFL/issues");
}

PlayMenu::PlayMenu() :UI("Play") {
    get<TextBox>("IP")->setTextAlignment(Font::Justify::ALIGN_VCENTER_LEFT);
    RakNet::SocketDescriptor SD;
    auto res = mPeer.Startup(1, &SD, 1);
    update();
}

void PlayMenu::event(Control * control, Event evt) {
    CHECKRET();
    auto name = std::string(control->getId() + 1);
    bool res;
    if (evt == Event::PRESS && (*control->getId()) == '@') {
        localClient = std::make_unique<Client>(name, res);
        if (res) {
            localClient->changeGroup(1);
            push<ClientMenu>();
        }
        else {
            localClient.reset();
            mServers.erase(name);
        }
    }
    if (evt == Event::PRESS && (*control->getId()) == '#') {
        localServer = std::make_unique<Server>(name);
        localClient = std::make_unique<Client>("127.0.0.1", res);
        if (res) {
            localClient->changeGroup(1);
            push<ServerMenu>();
        }
        else {
            localClient.reset();
            localServer.reset();
        }
    }
    if (evt == Event::PRESS && CMPID("IP"))
        Game::getInstance()->displayKeyboard(true);
    if (evt == Event::PRESS && CMPID("connect")) {
        localClient = std::make_unique<Client>(get<TextBox>("IP")->getText(), res);
        if (res) {
            localClient->changeGroup(1);
            push<ClientMenu>();
        }
        else localClient.reset();
    }
}

void PlayMenu::update(float flag) {
    static clock_t t = 0;
    mPeer.Ping("255.255.255.255", 23333, false);
    if (clock() - t > 2000 || flag == 0.0f) {
        using FS = FileSystem;
        {
            mServers.clear();
            for (auto packet = mPeer.Receive(); packet; mPeer.DeallocatePacket(packet), packet = mPeer.Receive()) {
                if (packet->data[0] == ID_UNCONNECTED_PONG)
                    mServers.insert(packet->systemAddress.ToString(false));
            }
            auto c = get<Container>("servers");
            const_cast<std::vector<Control*>&>(c->getControls()).clear();
            for (auto&& x : mServers) {
                auto button = Button::create(("@" + x).c_str());
                button->setText(x.c_str());
                button->addListener(this, Event::PRESS);
                c->addControl(button);
            }
        }
        {
            std::vector<std::string> maps;
            listDirs("res/maps", maps);
            auto c = get<Container>("maps");
            const_cast<std::vector<Control*>&>(c->getControls()).clear();
            for (auto&& x : maps) {
                auto button = Button::create(("#" + x).c_str());
                button->setText(x.c_str());
                button->addListener(this, Event::PRESS);
                c->addControl(button);
            }
        }

        t = clock();
    }
}

PlayMenu:: ~PlayMenu() {
    mPeer.Shutdown(0);
}

SettingsMenu::SettingsMenu() :UI("Settings") { read(); }

extern void unpackAllPacks();

void SettingsMenu::event(Control * control, Event evt) {
    CHECKRET();
    if (evt == Event::PRESS && CMPID("currentsize")) {
        get<Slider>("width")->setValue(Game::getInstance()->getWidth());
        get<Slider>("height")->setValue(Game::getInstance()->getHeight());
    }
    if (evt == Event::PRESS && CMPID("reload")) {
        unpackAllPacks();
        globalUnits.clear();
        loadAllUnits();
        globalBullets.clear();
        loadAllBullets();
    }
}

void writeSettings() {
    std::stringstream ss;
    ss << "shadowSize=" << shadowSize << std::endl
        << "enableParticle=" << std::boolalpha << enableParticle << std::endl;
    auto str = ss.str();
    uniqueRAII<Stream> file = FileSystem::open("game.info", FileSystem::WRITE);
    file->write(str.data(), str.size(), 1);
}

void SettingsMenu::read() {
    auto p = Game::getInstance()->getConfig()->getNamespace("window", true);
    get<CheckBox>("fullscreen")->setChecked(p->getBool("fullscreen", false));
    get<Slider>("width")->setValue(p->getInt("width"));
    get<Slider>("height")->setValue(p->getInt("height"));
    get<CheckBox>("particle")->setChecked(enableParticle);
    get<Slider>("shadow")->setValue(shadowSize-shadowSize%512);
}

SettingsMenu::~SettingsMenu() {
    {
        std::stringstream out;
        out << std::boolalpha <<
            R"(
ui
{
    theme = res/default.theme
}

window
{
    title = The First Law
    resizable = true
    samples = 8
    width = )" << get<Slider>("width")->getValue() <<
            "\n    height = " << get<Slider>("height")->getValue() <<
            "\n    fullscreen = " << get<CheckBox>("fullscreen")->isChecked() <<
            "\n}\n";
        auto str = out.str();
        uniqueRAII<Stream> file = FileSystem::open("game.config", FileSystem::StreamMode::WRITE);
        file->write(str.c_str(), sizeof(char), str.size());
    }

    auto p = Game::getInstance()->getConfig()->getNamespace("window", true);
    p->setString("fullscreen", get<CheckBox>("fullscreen")->isChecked() ? "true" : "false");
    p->setString("width", to_string<size_t>(get<Slider>("width")->getValue()).c_str());
    p->setString("height", to_string<size_t>(get<Slider>("height")->getValue()).c_str());
    enableParticle = get<CheckBox>("particle")->isChecked();
    shadowSize = get<Slider>("shadow")->getValue();
    if (!shadowSize)shadowSize = 1;
    writeSettings();
}

ServerMenu::ServerMenu() :UI("Server") {
    get<Label>("IP")->setText(("Your local IP is " + localServer->getIP()).c_str());
    get<Label>("IP")->setTextColor({ 0.0f,1.0f,0.0f,1.0f });
}

void ServerMenu::event(Control * control, Event evt) {
    CHECKRET();
    if (evt == Event::PRESS && CMPID("run")) {
        if (get<CheckBox>("ai")->isChecked()) {
            bool flag = false;
            aiFuture =std::make_unique<std::future<void>>(std::async(std::launch::async, AIMain, &flag));
            while (!flag) localServer->waitClient();
        }
        localServer->run();
        while (localClient->wait() != Client::WaitResult::Go)
            std::this_thread::yield();
        push<GameMain>();
    }
}

void ServerMenu::update(float) {
    localServer->waitClient();
    auto& info = localServer->getClientInfo();
    std::stringstream ss;
    ss << "The number of clients is " << info.size() << std::endl;
    uint16_t count[5]{};
    for (auto&& x : info)
        ++count[x.second.group];
    for (uint16_t i = 1; i <= 4; ++i)
        if (count[i])
            ss << "Group " << i << ":" << count[i] << std::endl;
    get<Label>("state")->setText(ss.str().c_str());
    localClient->wait();
}

ServerMenu::~ServerMenu() {
    localClient.reset();
    localServer.reset();
}

GameMain::GameMain() :UI("GameMain") {
    auto c = get<Container>("units");
#ifdef ANDROID
    {
        auto bs = Container::create("c");
        bs->setHeight(0.1f, true);
        bs->setWidth(1.0f, true);
        bs->setLayout(Layout::LAYOUT_ABSOLUTE);
        auto u = Button::create("up"); u->setText("Up");
        u->addListener(this, PRESS); u->setHeight(1.0f, true);
        u->setWidth(0.5f, true); u->setAlignment(Control::Alignment::ALIGN_LEFT);
        auto d = Button::create("down"); d->setText("Down");
        d->addListener(this, PRESS); d->setHeight(1.0f, true);
        d->setWidth(0.5f, true); d->setAlignment(Control::Alignment::ALIGN_RIGHT);
        bs->addControl(u);
        bs->addControl(d);
        mForm->insertControl(bs, 2);
        c->setHeight(0.4f, true);
        auto b = Button::create("cancel");
        b->setText("Cancel");
        b->setWidth(1.0f, true);
        b->setHeight(0.1f, true);
        mForm->addControl(b);
    }
#endif // ANDROID
    for (auto&& u : globalUnits) {
        auto button = Button::create("");
        button->setText(u.first.c_str());
        button->setWidth(1.0, true);
        button->setTextAlignment(Font::Justify::ALIGN_VCENTER_HCENTER);
        button->addListener(this, Event::PRESS);
        c->addControl(button);
    }
    mCurrent = globalUnits.cbegin()->first;
    get<Label>("weight")->setText(("Weight " + mCurrent).c_str());
}

void GameMain::event(Control * control, Event evt) {
    CHECKRET();
    if (evt == Event::PRESS && CMPID("")) {
        mCurrent = dynamic_cast<Button*>(control)->getText();
        get<Slider>("weight")->setText(("Weight " + mCurrent).c_str());
    }
    else if (evt == Event::VALUE_CHANGED && CMPID("weight"))
        localClient->changeWeight(mCurrent, dynamic_cast<Slider*>(control)->getValue());
#ifdef ANDROID
    else if (evt == Event::PRESS && CMPID("up"))
        localClient->scaleEvent(100.0f);
    else if (evt == Event::PRESS && CMPID("down"))
        localClient->scaleEvent(-100.0f);
    else if (evt == Event::PRESS && CMPID("cancel"))
        localClient->cancel();
#endif // ANDROID
}

void GameMain::update(float delta) {
    get<Label>("state")->setText(("FPS :" + to_string(Game::getInstance()->getFrameRate()) +
        " Unit:" + to_string(localClient->getUnitNum())).c_str());
    if (localServer)localServer->update(delta);
    localClient->setViewport(mForm->getWidth());
    if (localClient->update(delta))
        get<Slider>("weight")->setValue(localClient->getWeight(mCurrent));
    else
        pop();
}

GameMain::~GameMain() {
    localClient->stop();
    if (localServer)localServer->stop();
    aiFuture.reset();
}

ClientMenu::ClientMenu() :UI("Client") {}

void ClientMenu::event(Control * control, Event evt) {
    CHECKRET();
    if (evt == Event::VALUE_CHANGED && CMPID("group"))
        localClient->changeGroup(static_cast<uint8_t>(get<Slider>("group")->getValue()));
}

void ClientMenu::update(float) {
    auto res = localClient->wait();
    switch (res) {
    case Client::WaitResult::Disconnected:pop();
        break;
    case Client::WaitResult::Go:push<GameMain>();
        break;
    default:
        break;
    }
}

ClientMenu::~ClientMenu() {
    localClient.reset();
}


#undef CHECKRET
#undef CMPID

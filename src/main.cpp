#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <fstream>

using namespace geode::prelude;

namespace {
	struct Sample {
		float t, x, y;
	};

	bool modEnabled() {
		return Mod::get()->getSettingValue<bool>("enabled");
	}

	std::filesystem::path ghostPath(int levelID) {
		return Mod::get()->getSaveDir() / fmt::format("{}.ghost", levelID);
	}

	bool loadGhost(int levelID, float& percent, std::vector<Sample>& out) {
		std::ifstream f(ghostPath(levelID), std::ios::binary);
		if (!f) return false;
		uint32_t n = 0;
		f.read(reinterpret_cast<char*>(&percent), sizeof(float));
		f.read(reinterpret_cast<char*>(&n), sizeof(n));
		if (!f || n == 0 || n > 5'000'000) return false;
		out.resize(n);
		f.read(reinterpret_cast<char*>(out.data()), n * sizeof(Sample));
		return static_cast<bool>(f);
	}

	void saveGhost(int levelID, float percent, std::vector<Sample> const& data) {
		std::ofstream f(ghostPath(levelID), std::ios::binary | std::ios::trunc);
		if (!f) return;
		uint32_t n = static_cast<uint32_t>(data.size());
		f.write(reinterpret_cast<char const*>(&percent), sizeof(float));
		f.write(reinterpret_cast<char const*>(&n), sizeof(n));
		f.write(reinterpret_cast<char const*>(data.data()), n * sizeof(Sample));
	}

	void applyOpacity(CCNode* node, GLubyte opacity) {
		if (auto rgba = typeinfo_cast<CCRGBAProtocol*>(node)) {
			rgba->setOpacity(opacity);
		}
		if (node->getChildrenCount() == 0) return;
		for (auto child : CCArrayExt<CCNode*>(node->getChildren())) {
			applyOpacity(child, opacity);
		}
	}
}

class $modify(BRPlayLayer, PlayLayer) {
	struct Fields {
		std::vector<Sample> recording;
		std::vector<Sample> best;
		float bestFilePercent = 0.f;
		float time = 0.f;
		float lastRecT = -1.f;
		size_t ghostIdx = 0;
		GLubyte lastOpacity = 0;
		CCNode* lineNode = nullptr;
		CCLabelBMFont* lineLabel = nullptr;
		SimplePlayer* ghost = nullptr;
	};

	// ---------- Linie ----------
	void refreshLine() {
		auto f = m_fields.self();
		if (f->lineNode) {
			f->lineNode->removeFromParent();
			f->lineNode = nullptr;
			f->lineLabel = nullptr;
		}
		if (m_level->isPlatformer()) return;
		int pct = m_level->m_normalPercent.value();
		if (pct <= 0 || pct > 100) return;

		float x = m_levelLength * static_cast<float>(pct) / 100.f;

		auto node = CCNode::create();
		auto draw = CCDrawNode::create();
		ccColor4F col = { 1.f, 1.f, 1.f, 0.8f };
		// Sehr lange Linie, damit sie von ganz unten bis ganz oben reicht
		for (float y = -2000.f; y < 8000.f; y += 30.f) {
			draw->drawSegment({ x, y }, { x, y + 18.f }, 0.8f, col);
		}
		node->addChild(draw);

		auto label = CCLabelBMFont::create(fmt::format("{}%", pct).c_str(), "bigFont.fnt");
		label->setScale(0.4f);
		label->setOpacity(200);
		label->setPosition({ x, 330.f });
		node->addChild(label);

		m_objectLayer->addChild(node, 1000);
		f->lineNode = node;
		f->lineLabel = label;
	}

	// ---------- Geist ----------
	void createGhost() {
		auto f = m_fields.self();
		if (m_level->isPlatformer()) return;
		int id = m_level->m_levelID.value();
		if (id == 0) return;
		if (!loadGhost(id, f->bestFilePercent, f->best)) return;

		auto gm = GameManager::get();
		auto ghost = SimplePlayer::create(gm->getPlayerFrame());
		ghost->setColor(gm->colorForIdx(gm->getPlayerColor()));
		ghost->setSecondColor(gm->colorForIdx(gm->getPlayerColor2()));
		ghost->setVisible(false);
		m_objectLayer->addChild(ghost, 500);
		f->ghost = ghost;
	}

	// ---------- Speichern ----------
	void storeIfBest(float pct) {
		auto f = m_fields.self();
		if (m_isPracticeMode || m_isTestMode || m_level->isPlatformer()) return;
		int id = m_level->m_levelID.value();
		if (id == 0) return;
		if (f->recording.size() < 2) return;
		if (pct <= f->bestFilePercent + 0.001f) return;

		saveGhost(id, pct, f->recording);
		f->bestFilePercent = pct;
		f->best = f->recording;
	}

	// ---------- Hooks ----------
	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();
		refreshLine();
		createGhost();
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		auto f = m_fields.self();
		f->recording.clear();
		f->time = 0.f;
		f->lastRecT = -1.f;
		f->ghostIdx = 0;
		refreshLine();
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		float pct = this->getCurrentPercent();
		bool wasDead = m_player1->m_isDead;
		PlayLayer::destroyPlayer(player, object);
		if (player == m_player1 && !wasDead && m_player1->m_isDead) {
			storeIfBest(pct);
		}
	}

	void levelComplete() {
		storeIfBest(100.f);
		PlayLayer::levelComplete();
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		auto f = m_fields.self();
		auto mod = Mod::get();

		bool on = modEnabled();
		bool showLine = on && mod->getSettingValue<bool>("show-line");
		bool showGhost = on && mod->getSettingValue<bool>("show-ghost");

		if (f->lineNode) {
			f->lineNode->setVisible(showLine);
			if (f->lineLabel) {
				// Prozentzahl liegt auf der Linie und bleibt immer am oberen
				// Bildschirmrand, egal wie hoch die Kamera steigt.
				auto winSize = CCDirector::get()->getWinSize();
				auto topInLayer = m_objectLayer->convertToNodeSpace({ 0.f, winSize.height - 25.f });
				f->lineLabel->setPositionY(topInLayer.y);
			}
		}

		if (m_player1->m_isDead) return;
		f->time += dt;

		// Aufnahme (ca. 60 Samples pro Sekunde)
		if (!m_isPracticeMode && !m_isTestMode) {
			if (f->lastRecT < 0.f || f->time - f->lastRecT >= 1.f / 60.f) {
				f->recording.push_back({ f->time, m_player1->getPositionX(), m_player1->getPositionY() });
				f->lastRecT = f->time;
			}
		}

		// Wiedergabe des Geists
		if (f->ghost) {
			if (!showGhost || f->best.size() < 2) {
				f->ghost->setVisible(false);
				return;
			}

			while (f->ghostIdx + 1 < f->best.size() && f->best[f->ghostIdx + 1].t <= f->time) {
				f->ghostIdx++;
			}

			if (f->ghostIdx + 1 >= f->best.size()) {
				f->ghost->setVisible(false); // Geist ist am Ende seines Runs angekommen
				return;
			}

			auto const& a = f->best[f->ghostIdx];
			auto const& b = f->best[f->ghostIdx + 1];
			float span = b.t - a.t;
			float k = span > 0.0001f ? std::clamp((f->time - a.t) / span, 0.f, 1.f) : 0.f;
			f->ghost->setPosition({ a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k });
			f->ghost->setVisible(true);

			float op = std::clamp(mod->getSettingValue<double>("ghost-opacity"), 0.05, 1.0);
			GLubyte o = static_cast<GLubyte>(op * 255.f);
			if (o != f->lastOpacity) {
				applyOpacity(f->ghost, o);
				f->lastOpacity = o;
			}
		}
	}
};

// ---------- Pause-Menü ----------
class $modify(BRPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto toggler = CCMenuItemToggler::createWithStandardSprites(
			this, menu_selector(BRPauseLayer::onToggleBestRun), 0.6f
		);
		toggler->toggle(modEnabled());

		auto label = CCLabelBMFont::create("Best Run", "bigFont.fnt");
		label->setScale(0.35f);
		label->setPosition({ toggler->getContentWidth() / 2.f, toggler->getContentHeight() + 8.f });
		toggler->addChild(label);

		if (auto menu = this->getChildByID("right-button-menu")) {
			menu->addChild(toggler);
			menu->updateLayout();
		} else {
			// Fallback, falls die Node-ID nicht existiert
			auto fallbackMenu = CCMenu::create();
			fallbackMenu->setPosition({ 30.f, 40.f });
			fallbackMenu->addChild(toggler);
			this->addChild(fallbackMenu);
		}
	}

	void onToggleBestRun(CCObject* sender) {
		auto toggler = static_cast<CCMenuItemToggler*>(sender);
		// isToggled() liefert hier noch den Zustand vor dem Klick
		Mod::get()->setSettingValue<bool>("enabled", !toggler->isToggled());
	}
};

// ---------- Tastenkombination ----------
// Hinweis: Je nach Geode-Version kann die Signatur des Callbacks leicht abweichen.
$on_mod(Loaded) {
	listenForKeybindSettingPresses("toggle-key", [](Keybind const&, bool down, bool repeat, double) {
		if (!down || repeat) return;
		if (!PlayLayer::get()) return;
		bool now = !modEnabled();
		Mod::get()->setSettingValue<bool>("enabled", now);
		Notification::create(
			now ? "Best Run: AN" : "Best Run: AUS",
			NotificationIcon::Info, 1.f
		)->show();
	});
}

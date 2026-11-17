#include "trajectory.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/GameToolbox.hpp>
#include <Geode/binding/PlayerCheckpoint.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {

using Clock = std::chrono::steady_clock;
constexpr float stepSeconds = 1.f / 240.f;
constexpr float playerDelta = stepSeconds * 60.f;
constexpr int previewSteps = 240;

struct Preview {
    Ref<CCDrawNode> drawing;
    Ref<PlayerObject> player;
    Ref<PlayerCheckpoint> checkpoint;
    Clock::time_point lastUpdate;
    std::vector<GameObject*> solids;
    std::vector<GameObject*> hazards;
    std::vector<GameObject*> candidates;
    std::unordered_set<GameObject*> broken;
};

Preview preview;

enum class End { None, Collision, Interaction };

struct Simulation {
    PlayLayer* play;
    End end = End::None;
};

Simulation* simulation = nullptr;

// Native collision checks use the layer's collision queues and clock as scratch space.
struct CollisionState {
    PlayLayer* play;
    double totalTime;
    double levelTime;
    unsigned int progress;
    int solidCount;
    int solidIndex;
    int hazardCount;
    int hazardIndex;
    bool debugDraw;
    bool disableHitbox;
    decltype(GameToolbox::getfast_srand()) randomSeed;

    explicit CollisionState(PlayLayer* layer)
        : play(layer), totalTime(layer->m_gameState.m_totalTime),
          levelTime(layer->m_gameState.m_levelTime), progress(layer->m_gameState.m_currentProgress),
          solidCount(layer->m_solidCollisionObjectsCount), solidIndex(layer->m_solidCollisionObjectsIndex),
          hazardCount(layer->m_hazardCollisionObjectsCount), hazardIndex(layer->m_hazardCollisionObjectsIndex),
          debugDraw(layer->m_isDebugDrawEnabled), disableHitbox(layer->m_disablePlayerHitbox),
          randomSeed(GameToolbox::getfast_srand()) {
        play->m_solidCollisionObjects.swap(preview.solids);
        play->m_hazardCollisionObjects.swap(preview.hazards);
        play->m_isDebugDrawEnabled = false;
        play->m_disablePlayerHitbox = false;
    }

    void startBranch() const {
        restoreClock();
        play->m_solidCollisionObjectsCount = 0;
        play->m_hazardCollisionObjectsCount = 0;
        play->m_solidCollisionObjectsIndex = static_cast<int>(play->m_solidCollisionObjects.size());
        play->m_hazardCollisionObjectsIndex = static_cast<int>(play->m_hazardCollisionObjects.size());
        GameToolbox::fast_srand(randomSeed);
    }

    ~CollisionState() {
        play->m_solidCollisionObjects.swap(preview.solids);
        play->m_hazardCollisionObjects.swap(preview.hazards);
        play->m_solidCollisionObjectsCount = solidCount;
        play->m_hazardCollisionObjectsCount = hazardCount;
        play->m_solidCollisionObjectsIndex = solidIndex;
        play->m_hazardCollisionObjectsIndex = hazardIndex;
        play->m_isDebugDrawEnabled = debugDraw;
        play->m_disablePlayerHitbox = disableHitbox;
        restoreClock();
        GameToolbox::fast_srand(randomSeed);
    }

    void restoreClock() const {
        play->m_gameState.m_totalTime = totalTime;
        play->m_gameState.m_levelTime = levelTime;
        play->m_gameState.m_currentProgress = progress;
    }
};

bool isGeometry(GameObjectType type) {
    using enum GameObjectType;
    return type == Solid || type == Breakable || type == Slope || type == Hazard || type == AnimatedHazard;
}

bool changesPhysics(GameObject* object, PlayerObject* player) {
    using enum GameObjectType;
    switch (object->m_objectType) {
        case Solid:
        case Breakable:
        case Slope:
        case Hazard:
        case AnimatedHazard:
        case Decoration:
        case SecretCoin:
        case UserCoin:
        case Collectible:
        case EnterEffectObject:
        case CollisionObject:
            return false;
        case Modifier:
        case Special:
            return object->isSpeedObject();
        case YellowJumpRing:
        case PinkJumpRing:
        case GravityRing:
        case GreenRing:
        case DropRing:
        case RedJumpRing:
        case CustomRing:
        case DashRing:
        case GravityDashRing:
        case SpiderOrb:
        case TeleportOrb: {
            const auto button = player->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
            return player->m_jumpBuffered || (button != player->m_holdingButtons.end() && button->second);
        }
        default:
            return true;
    }
}

void drawEndpoint(CCRect rect, float width, ccColor4F color) {
    CCPoint points[] = {
        {rect.getMinX(), rect.getMinY()}, {rect.getMinX(), rect.getMaxY()},
        {rect.getMaxX(), rect.getMaxY()}, {rect.getMaxX(), rect.getMinY()},
    };
    preview.drawing->drawPolygon(points, 4, {0.f, 0.f, 0.f, 0.f}, width, color);
}

void loadPlayer(PlayerObject& source, PlayerObject& player) {
    player.stopAllActions();
    player.copyAttributes(&source);
    player.loadFromCheckpoint(preview.checkpoint);
    player.setPosition(source.getPosition());
    player.setRotation(source.getRotation());
    player.m_yVelocity = source.m_yVelocity;
    player.m_holdingButtons = source.m_holdingButtons;
    player.m_isSecondPlayer = source.m_isSecondPlayer;
    player.m_isOnGround2 = source.m_isOnGround2;
    player.m_isOnGround3 = source.m_isOnGround3;
    player.m_isOnGround4 = source.m_isOnGround4;
    player.m_lastActivatedPortal = source.m_lastActivatedPortal;
    player.m_jumpBuffered = source.m_jumpBuffered;
    player.m_shouldTryPlacingCheckpoint = false;
    player.m_playEffects = false;
    player.m_isDead = false;
    player.setVisible(false);
    player.resetCollisionLog(true);
}

void drawBranch(Simulation& run, CollisionState const& state, PlayerObject& source,
    bool hold, ccColor4F color, float width) {
    state.startBranch();
    preview.broken.clear();
    run.end = End::None;
    auto& player = *preview.player.data();
    loadPlayer(source, player);

    const auto button = static_cast<int>(PlayerButton::Jump);
    const auto held = source.m_holdingButtons.find(button);
    if (hold && (held == source.m_holdingButtons.end() || !held->second)) {
        player.pushButton(PlayerButton::Jump);
    } else if (!hold) {
        player.releaseButton(PlayerButton::Jump);
        player.m_jumpBuffered = false;
    }

    auto drawn = player.getPosition();
    for (int step = 0; step < previewSteps && run.end == End::None; ++step) {
        const auto before = player.getPosition();
        run.play->m_gameState.m_totalTime += stepSeconds;
        run.play->m_gameState.m_levelTime += stepSeconds;
        ++run.play->m_gameState.m_currentProgress;
        player.m_totalTime += stepSeconds;
        player.resetCollisionLog(false);
        player.update(playerDelta);
        if (run.end == End::None && run.play->checkCollisions(&player, playerDelta, false) == 1) {
            run.end = End::Collision;
        }
        const auto after = player.getPosition();
        const auto movement = after - before;
        if (!std::isfinite(after.x) || !std::isfinite(after.y)) break;
        if (movement.x * movement.x + movement.y * movement.y > 60.f * 60.f) {
            drawn = after; // A spider teleport is discontinuous.
        }
        if (step % 3 == 2 || step == previewSteps - 1 || run.end != End::None) {
            if (drawn.x != after.x || drawn.y != after.y) {
                preview.drawing->drawSegment(drawn, after, width, color);
            }
            drawn = after;
        }
        if (run.end != End::None) {
            const ccColor4F contact = run.end == End::Collision
                ? ccColor4F{1.f, 0.25f, 0.18f, 1.f} : ccColor4F{1.f, 0.78f, 0.25f, 0.95f};
            drawEndpoint(player.getObjectRect(), width, contact);
        }
    }
    player.setVisible(false);
}

} // namespace

void trajectory::update(PlayLayer* play, bool enabled) {
    if (!enabled || !play->m_started || !play->m_objectLayer || play->m_isPaused
        || play->m_hasCompletedLevel || play->m_isPlatformer || !play->m_player1 || play->m_player1->m_isDead) {
        clear();
        return;
    }

    const auto now = Clock::now();
    if (now - preview.lastUpdate < std::chrono::milliseconds(25)) return;
    preview.lastUpdate = now;
    if (!preview.drawing) {
        preview.drawing = CCDrawNode::create();
        if (!preview.drawing) return;
        play->m_objectLayer->addChild(preview.drawing, 1000);
    }
    preview.drawing->clear();
    if (!preview.player) {
        preview.player = PlayerObject::create(1, 1, play, play, true);
        preview.checkpoint = Ref<PlayerCheckpoint>::adopt(new PlayerCheckpoint());
        if (!preview.player || !preview.checkpoint->init()) {
            preview.player = nullptr;
            preview.checkpoint = nullptr;
            return;
        }
        preview.player->setVisible(false);
        play->m_objectLayer->addChild(preview.player);
    }

    const auto zoom = std::max(play->m_gameState.m_cameraZoom, 0.1f);
    CollisionState state(play);
    Simulation run{play};
    simulation = &run;
    PlayerObject* sources[] = {play->m_player1, play->m_gameState.m_isDualMode ? play->m_player2 : nullptr};
    for (auto* source : sources) {
        if (!source || source->m_isDead) continue;
        source->saveToCheckpoint(preview.checkpoint);
        drawBranch(run, state, *source, false, {0.15f, 0.9f, 1.f, 0.85f}, 0.42f / zoom);
        drawBranch(run, state, *source, true, {1.f, 0.25f, 0.65f, 0.95f}, 0.28f / zoom);
    }
    simulation = nullptr;
}

void trajectory::clear() {
    if (preview.drawing) preview.drawing->clear();
    preview.lastUpdate = {};
}

void trajectory::reset() {
    if (preview.drawing) preview.drawing->removeFromParentAndCleanup(true);
    if (preview.player) preview.player->removeFromParentAndCleanup(true);
    preview = {};
}

bool trajectory::interceptDeath(PlayLayer* play) {
    if (!simulation || simulation->play != play) return false;
    simulation->end = End::Collision;
    return true;
}

class $modify(TrajectoryCollisionLayer, GJBaseGameLayer) {
    void collisionCheckObjects(PlayerObject* player, std::vector<GameObject*>* objects, int count, float dt) {
        if (!simulation || player != preview.player) {
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }
        if (!objects || count <= 0) return;

        const auto playerRect = player->getObjectRect();
        auto& candidates = preview.candidates;
        candidates.clear();
        const auto limit = std::min(objects->size(), static_cast<size_t>(count));
        for (size_t i = 0; i < limit; ++i) {
            auto* object = (*objects)[i];
            if (!object || object == m_anticheatSpike || object->m_isDisabled || object->m_isDisabled2
                || object->m_isGroupDisabled || object->m_isNoTouch || preview.broken.contains(object)) continue;

            if (isGeometry(object->m_objectType)) {
                candidates.push_back(object);
            } else if (simulation->end == End::None && object != player->m_lastActivatedPortal
                && changesPhysics(object, player) && playerRect.intersectsRect(object->getObjectRect())) {
                auto* interactive = typeinfo_cast<EnhancedGameObject*>(object);
                if (!interactive || !(player->m_isSecondPlayer
                    ? interactive->m_activatedByPlayer2 : interactive->m_activatedByPlayer1)) {
                    simulation->end = End::Interaction;
                }
            }
        }
        if (!candidates.empty()) {
            GJBaseGameLayer::collisionCheckObjects(player, &candidates, static_cast<int>(candidates.size()), dt);
        }
    }

    void destroyObject(GameObject* object) {
        if (simulation) preview.broken.insert(object);
        else GJBaseGameLayer::destroyObject(object);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (!simulation) GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (!simulation) GJBaseGameLayer::flipGravity(player, flip, noEffects);
        else if (player == preview.player) player->flipGravity(flip, true);
    }
};

class $modify(TrajectoryPlayer, PlayerObject) {
    void incrementJumps() {
        if (!simulation) PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (!simulation) PlayerObject::playSpiderDashEffect(from, to);
    }
};

class $modify(TrajectoryTrail, HardStreak) {
    void addPoint(CCPoint position) {
        if (!simulation) HardStreak::addPoint(position);
    }
};

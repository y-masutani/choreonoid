#include "LinkKinematicsKitManager.h"
#include "BodySelectionManager.h"
#include <cnoid/Link>
#include <cnoid/LinkKinematicsKit>
#include <cnoid/JointPath>
#include <cnoid/CompositeIK>
#include <cnoid/CoordinateFrameList>
#include <cnoid/LinkCoordFrameSetSuite>
#include <cnoid/LinkCoordFrameListSuiteItem>
#include <cnoid/RootItem>
#include <cnoid/BodyItem>
#include <cnoid/WorldItem>
#include <cnoid/ItemList>
#include <cnoid/PositionEditManager>
#include <cnoid/PositionDragger>
#include <cnoid/ConnectionSet>
#include <cnoid/ValueTree>
#include <map>

using namespace std;
using namespace cnoid;

namespace {

enum FrameType {
    WorldFrame = LinkKinematicsKit::WorldFrame,
    BodyFrame = LinkKinematicsKit::BodyFrame,
    LinkFrame = LinkKinematicsKit::LinkFrame
};

}
    
namespace cnoid {

class LinkKinematicsKitManager::Impl
{
public:
    BodyItem* bodyItem;

    // Use an integer value as a key to keep the number of instances growing
    map<int, LinkKinematicsKitPtr> linkIndexToKinematicsKitMap;

    ScopedConnection bodyItemConnection;
    ScopedConnection frameListConnection;
    LinkCoordFrameSetSuitePtr commonFrameSetSuite;

    BodySelectionManager* bodySelectionManager;
    AbstractPositionEditTarget* frameEditTarget;
    Link* frameEditLink;
    PositionDraggerPtr positionDragger;
    SgUpdate update;
    ScopedConnection positionEditManagerConnection;
    ScopedConnectionSet frameEditConnections;

    Impl(BodyItem* bodyItem);
    std::shared_ptr<InverseKinematics> findPresetIK(Link* targetLink);
    void onBodyItemPositionChanged();
    void findFrameListSuiteItem();
    void onFrameListSuiteItemAddedOrUpdated(LinkCoordFrameListSuiteItem* frameListSuiteItem);
    void setupPositionDragger();
    bool onPositionEditRequest(AbstractPositionEditTarget* target);
    bool startBodyFrameEditing(AbstractPositionEditTarget* target, CoordinateFrame* frame);
    bool startLinkFrameEditing(AbstractPositionEditTarget* target, CoordinateFrame* frame);
    void setFrameEditTarget(AbstractPositionEditTarget* target, Link* link);
    void onFrameEditPositionChanged(const Position& T);
    void onDraggerPositionChanged();
};

}


LinkKinematicsKitManager::LinkKinematicsKitManager(BodyItem* bodyItem)
{
    impl = new Impl(bodyItem);
}


LinkKinematicsKitManager::Impl::Impl(BodyItem* bodyItem)
    : bodyItem(bodyItem)
{
    bodyItemConnection =
        bodyItem->sigPositionChanged().connect(
            [&](){ onBodyItemPositionChanged(); });
    
    bodySelectionManager = BodySelectionManager::instance();
    
    commonFrameSetSuite = new LinkCoordFrameSetSuite;
    findFrameListSuiteItem();

    frameListConnection =
        LinkCoordFrameListSuiteItem::sigInstanceAddedOrUpdated().connect(
            [&](LinkCoordFrameListSuiteItem* frameListSuiteItem){
                onFrameListSuiteItemAddedOrUpdated(frameListSuiteItem);
            });

    setupPositionDragger();
}


LinkKinematicsKitManager::~LinkKinematicsKitManager()
{
    delete impl;
}


LinkKinematicsKit* LinkKinematicsKitManager::findKinematicsKit(Link* targetLink)
{
    if(!targetLink){
        targetLink = impl->bodyItem->body()->findUniqueEndLink();
        if(!targetLink){
            return nullptr;
        }
    }
    
    LinkKinematicsKit* kit = nullptr;
    
    auto iter = impl->linkIndexToKinematicsKitMap.find(targetLink->index());
    if(iter != impl->linkIndexToKinematicsKitMap.end()){
        auto foundKit = iter->second;
        if(foundKit->link() == targetLink){
            kit = foundKit;
        }
    }

    if(!kit){
        // A link kinematics kit can be created only for a link path
        // included in the preset inverse kinematics paths
        if(auto presetIK = impl->findPresetIK(targetLink)){
            kit = new LinkKinematicsKit(targetLink);
            kit->setInversetKinematics(presetIK);
            kit->setFrameSetSuite(impl->commonFrameSetSuite);
            impl->linkIndexToKinematicsKitMap[targetLink->index()] = kit;
        }
    }

    return kit;        
}


std::shared_ptr<InverseKinematics> LinkKinematicsKitManager::Impl::findPresetIK(Link* targetLink)
{
    std::shared_ptr<InverseKinematics> ik;
    auto body = bodyItem->body();
    const Mapping& setupMap = *body->info()->findMapping("defaultIKsetup");
    if(setupMap.isValid()){
        const Listing& setup = *setupMap.findListing(targetLink->name());
        if(setup.isValid() && !setup.empty()){
            Link* baseLink = body->link(setup[0].toString());
            if(baseLink){
                if(setup.size() == 1){
                    ik = JointPath::getCustomPath(body, baseLink, targetLink);
                } else {
                    auto compositeIK = make_shared<CompositeIK>(body, targetLink);
                    ik = compositeIK;
                    for(int i=0; i < setup.size(); ++i){
                        Link* baseLink = body->link(setup[i].toString());
                        if(baseLink){
                            if(!compositeIK->addBaseLink(baseLink)){
                                ik.reset();
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    return ik;
}


void LinkKinematicsKitManager::Impl::onBodyItemPositionChanged()
{
    commonFrameSetSuite->resetFrameSets();
    findFrameListSuiteItem();
}


void LinkKinematicsKitManager::Impl::findFrameListSuiteItem()
{
    if(auto suiteItem = bodyItem->findItem<LinkCoordFrameListSuiteItem>()){
        *commonFrameSetSuite = *suiteItem->frameSetSuite();
    }
}


void LinkKinematicsKitManager::Impl::onFrameListSuiteItemAddedOrUpdated
(LinkCoordFrameListSuiteItem* frameListSuiteItem)
{
    bool isTargetFrameList = false;
    auto upperItem = frameListSuiteItem->parentItem();
    while(upperItem){
        if(auto upperBodyItem = dynamic_cast<BodyItem*>(upperItem)){
            if(upperBodyItem == bodyItem){
                isTargetFrameList = true;
            }
            break;
        }
        upperItem = upperItem->parentItem();
    }
    if(isTargetFrameList){
        *commonFrameSetSuite = *frameListSuiteItem->frameSetSuite();
        for(auto& kv : linkIndexToKinematicsKitMap){
            auto& kit = kv.second;
            kit->notifyFrameUpdate();
        }
    }
}


SgNode* LinkKinematicsKitManager::scene()
{
    return impl->positionDragger;
}


void LinkKinematicsKitManager::Impl::setupPositionDragger()
{
    positionDragger = new PositionDragger(PositionDragger::AllAxes, PositionDragger::PositiveOnlyHandle);
    positionDragger->setOverlayMode(true);
    positionDragger->setConstantPixelSizeMode(true, 92.0);
    positionDragger->setDisplayMode(PositionDragger::DisplayNever);

    positionDragger->sigPositionDragged().connect(
        [&](){ onDraggerPositionChanged(); });

    frameEditTarget = nullptr;

    positionEditManagerConnection =
        PositionEditManager::instance()->sigPositionEditRequest().connect(
            [&](AbstractPositionEditTarget* target){
                return onPositionEditRequest(target); });
}    


bool LinkKinematicsKitManager::Impl::onPositionEditRequest(AbstractPositionEditTarget* target)
{
    bool accepted = false;
    if(auto frame = dynamic_cast<CoordinateFrame*>(target->getPositionObject())){
        if(auto frameSet = frame->ownerFrameSet()){
            if(commonFrameSetSuite->frameSet(BodyFrame)->contains(frameSet)){
                accepted = startBodyFrameEditing(target, frame);
            } else if(commonFrameSetSuite->frameSet(LinkFrame)->contains(frameSet)){
                accepted = startLinkFrameEditing(target, frame);
            }
        }
    }
    return accepted;
}


bool LinkKinematicsKitManager::Impl::startBodyFrameEditing
(AbstractPositionEditTarget* target, CoordinateFrame* frame)
{
    if(auto link = bodySelectionManager->currentLink()){
        if(auto kit = bodyItem->findLinkKinematicsKit(link)){
            if(auto baseLink = kit->baseLink()){
                setFrameEditTarget(target, baseLink);
                return true;
            }
        }
    }
    return false;
}


bool LinkKinematicsKitManager::Impl::startLinkFrameEditing
(AbstractPositionEditTarget* target, CoordinateFrame* frame)
{
    auto endLink = bodyItem->body()->findUniqueEndLink();
    if(!endLink){
        endLink = bodySelectionManager->currentLink();
    }
    if(endLink){
        setFrameEditTarget(target, endLink);
        return true;
    }
    return false;
}


void LinkKinematicsKitManager::Impl::setFrameEditTarget(AbstractPositionEditTarget* target, Link* link)
{
    frameEditConnections.disconnect();

    frameEditTarget = target;
    frameEditLink = link;

    if(!target){
        positionDragger->setDisplayMode(PositionDragger::DisplayNever);

    } else {
        positionDragger->setDisplayMode(PositionDragger::DisplayAlways);
        positionDragger->setDragEnabled(target->isEditable());

        frameEditConnections.add(
            target->sigPositionChanged().connect(
                [&](const Position& T){ onFrameEditPositionChanged(T); }));

        frameEditConnections.add(
            target->sigPositionEditTargetExpired().connect(
                [=](){ setFrameEditTarget(nullptr, nullptr); }));

        frameEditConnections.add(
            bodyItem->sigKinematicStateChanged().connect(
                [&](){ onFrameEditPositionChanged(frameEditTarget->getPosition()); }));

        onFrameEditPositionChanged(target->getPosition());
    }
}


void LinkKinematicsKitManager::Impl::onFrameEditPositionChanged(const Position& T)
{
    Position F;
    F.linear() = frameEditLink->attitude();
    F.translation() = frameEditLink->translation();
    positionDragger->setPosition(F * T);
    positionDragger->notifyUpdate(update);
}


void LinkKinematicsKitManager::Impl::onDraggerPositionChanged()
{
    if(frameEditTarget){
        frameEditConnections.block();
        Position F;
        F.linear() = frameEditLink->attitude();
        F.translation() = frameEditLink->translation();
        Position T = F.inverse(Eigen::Isometry) * positionDragger->T();
        frameEditTarget->setPosition(T);
        frameEditConnections.unblock();
    }
}


bool LinkKinematicsKitManager::storeState(Mapping& archive) const
{
    auto body = impl->bodyItem->body();
    archive.setKeyQuoteStyle(DOUBLE_QUOTED);
    for(auto& kv : impl->linkIndexToKinematicsKitMap){
        int linkIndex = kv.first;
        if(auto link = body->link(linkIndex)){
            auto& kit = kv.second;
            MappingPtr state = new Mapping;
            if(!kit->storeState(*state)){
                return false;
            }
            if(!state->empty()){
                archive.insert(link->name(), state);
            }
        }
    }
    return true;
}


bool LinkKinematicsKitManager::restoreState(const Mapping& archive)
{
    auto body = impl->bodyItem->body();
    for(auto& kv : archive){
        auto& linkName = kv.first;
        if(auto link = body->link(linkName)){
            if(auto kit = findKinematicsKit(link)){
                kit->restoreState(*kv.second->toMapping());
            }
        }
    }
    return true;
}

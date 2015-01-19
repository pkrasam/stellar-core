#include "transactions/CreateOfferFrame.h"
#include "ledger/OfferFrame.h"
#include "util/Logging.h"
#include "util/types.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"

// This is pretty gnarly still. I'll clean it up as I write the tests
//
// convert from sheep to wheat
// selling sheep
// buying wheat

namespace stellar
{

using namespace std;

CreateOfferFrame::CreateOfferFrame(const TransactionEnvelope& envelope) : TransactionFrame(envelope)
{

}

// make sure these issuers exist and you can hold the ask currency
bool CreateOfferFrame::checkOfferValid(LedgerMaster& ledgerMaster)
{
    Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
    Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;

    

    if(sheep.type()!=NATIVE &&
        !ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.account, sheep, mSheepLineA))
    {   // we don't have what we are trying to sell
        mResultCode = txNOTRUST;
        return false;
    }

    if(wheat.type()!=NATIVE)
    {
        if(!ledgerMaster.getDatabase().loadTrustLine(mEnvelope.tx.account, wheat, mWheatLineA))
        {   // we can't hold what we are trying to buy
            mResultCode = txNOTRUST;
            return false;
        }

        if(!mWheatLineA.mEntry.trustLine().authorized)
        {   // we are not authorized to hold what we are trying to buy
            mResultCode = txNOT_AUTHORIZED;
            return false;
        }
    }
    return true;
}



// you are selling sheep for wheat
// need to check the counter offers selling wheat for sheep
// see if this is modifying an old offer
// see if this offer crosses any existing offers
bool CreateOfferFrame::doApply(LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    if(!checkOfferValid(ledgerMaster)) return false;
    Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
    Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;

    bool creatingNewOffer = false;
    uint32_t offerSeq = mEnvelope.tx.body.createOfferTx().sequence;
    // plus 1 since the account seq has already been incremented at this point
    if(offerSeq + 1 == mSigningAccount->mEntry.account().sequence)
    { // creating a new Offer
        creatingNewOffer = true;
        mSellSheepOffer.from(mEnvelope.tx);
    } else
    { // modifying an old offer
        
        if(ledgerMaster.getDatabase().loadOffer(mEnvelope.tx.account, offerSeq, mSellSheepOffer))
        {
            // make sure the currencies are the same
            if(!compareCurrency(mEnvelope.tx.body.createOfferTx().takerGets, mSellSheepOffer.mEntry.offer().takerGets) ||
                !compareCurrency(mEnvelope.tx.body.createOfferTx().takerPays, mSellSheepOffer.mEntry.offer().takerPays))
            {
                mResultCode = txMALFORMED;
                return false;
            }
        } else
        {
            mResultCode = txOFFER_NOT_FOUND;
            return false;
        }
    }

    mWheatTransferRate = getTransferRate(wheat, ledgerMaster);
    mSheepTransferRate = getTransferRate(sheep, ledgerMaster);


    int64_t maxSheepReceived = mEnvelope.tx.body.createOfferTx().amount;
    

    int64_t amountOfSheepOwned;
    if (sheep.type() == NATIVE)
    {
        amountOfSheepOwned = mSigningAccount->mEntry.account().balance -
            ledgerMaster.getMinBalance(mSigningAccount->mEntry.account().ownerCount);
    }
    else
    {
        amountOfSheepOwned = mSheepLineA.mEntry.trustLine().balance;
    }
    
    // amount of sheep for sale is the lesser of amount we have and amount put in the offer
    //int64_t amountOfSheepForSale = amountOfWheat*OFFER_PRICE_DIVISOR / sheepPrice;
    if (amountOfSheepOwned < maxSheepReceived)
    {
        maxSheepReceived = amountOfSheepOwned;
    }
    if (mSheepTransferRate != TRANSFER_RATE_DIVISOR)
    {
        maxSheepReceived = bigDivide(maxSheepReceived, mSheepTransferRate, TRANSFER_RATE_DIVISOR);
    }

    int64_t sheepPrice = mEnvelope.tx.body.createOfferTx().price;
    
    {
        soci::transaction sqlTx(ledgerMaster.getDatabase().getSession());
        LedgerDelta tempDelta;

        if (!convert(sheep, wheat,
            maxSheepReceived, sheepPrice, tempDelta, ledgerMaster))
        {
            return false;
        }



        if (mSellSheepOffer.mEntry.offer().amount > 0)
        { // we still have sheep to sell so leave an offer

            if (creatingNewOffer)
            {
                // make sure we don't allow us to add offers when we don't have the minbalance
                if (mSigningAccount->mEntry.account().balance <
                    ledgerMaster.getMinBalance(mSigningAccount->mEntry.account().ownerCount + 1))
                {
                    mResultCode = txBELOW_MIN_BALANCE;
                    return false;
                }

                mSellSheepOffer.storeAdd(tempDelta, ledgerMaster);

                mSigningAccount->mEntry.account().ownerCount++;
                mSigningAccount->storeChange(tempDelta, ledgerMaster);
            }
            else
            {
                mSellSheepOffer.storeChange(tempDelta, ledgerMaster);
            }
        }

        sqlTx.commit();
        delta.merge(tempDelta);
    }
    return true;
}

// must go through the available offers that we cross with
// If A is selling 10sheep at 2wheat per sheep and the transfer fee for wheat is .1
// transfer fee for sheep is .2
// B must send 2.2 wheat. A will send 1.2 sheep
// returns false if the tx should abort
bool CreateOfferFrame::convert(Currency& sheep,
    Currency& wheat, int64_t maxSheepReceived, int64_t minSheepPrice,
    LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    int64_t maxWheatPrice = bigDivide(OFFER_PRICE_DIVISOR,OFFER_PRICE_DIVISOR,minSheepPrice);

    int offerOffset = 0;
    while(maxSheepReceived > 0)
    {
        vector<OfferFrame> retList;
        ledgerMaster.getDatabase().loadBestOffers(5, offerOffset, wheat, sheep, retList);
        for(auto wheatOffer : retList)
        {
            if(wheatOffer.mEntry.offer().price > maxWheatPrice)
            { // wheat is too high!   
                return true;
            }

            if(wheatOffer.mEntry.offer().accountID == mSigningAccount->mEntry.account().accountID)
            {   // we are crossing our own offer
                mResultCode = txCROSS_SELF;
                return false;
            }

            int64_t numSheepReceived;
            if(!crossOffer(wheatOffer, maxSheepReceived, numSheepReceived, delta, ledgerMaster))
            {
                return false;
            }

            maxSheepReceived -= numSheepReceived;
        }
        // no more offers to load
        if(retList.size() < 5) return true;
        offerOffset += retList.size();
    }

    return true;
}

/*
# of sheep you have
# of sheep in offer
# of sheep you can send because of transfer rate

Objects we need to pull out of the DB:
accountA
trustLineSheepA
trustLineWheatA
accountB
trustLineSheepB
trustLineWheatB
issuerSheep
issuerWheat


in:
offer
# of wheat we want to buy
# of sheep we have to sell
transfer rate of sheep
transfer rate of wheat

out:
# of wheat we bought
# of sheep we sold
adjust all the balances

min(
amountSheepYouHave
amountSheepYouOffer
amountSheepTheyWant
amountWheatTheyHave
)

*/
// take up to amountToTake of the offer
// amountToTake is reduced by the amount taken
// returns false if there was an error
bool CreateOfferFrame::crossOffer(OfferFrame& sellingWheatOffer,
    int64_t maxSheepReceived, int64_t& amountSheepReceived,
    LedgerDelta& delta, LedgerMaster& ledgerMaster)
{
    Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
    Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;
    uint256& accountBID = sellingWheatOffer.mEntry.offer().accountID;
    
    
    AccountFrame accountB;
    if(!ledgerMaster.getDatabase().loadAccount(accountBID, accountB))
    {
        mResultCode = txINTERNAL_ERROR;
        return false;
    }
    
    TrustFrame wheatLineAccountB;
    if(wheat.type()!=NATIVE)
    {
        if(!ledgerMaster.getDatabase().loadTrustLine(accountBID,
            wheat, wheatLineAccountB))
        {
            mResultCode = txINTERNAL_ERROR;
            return false;
        }
    }
    

    TrustFrame sheepLineAccountB;
    if(sheep.type()!=NATIVE)
    {
        if(!ledgerMaster.getDatabase().loadTrustLine(accountBID,
            sheep, sheepLineAccountB))
        {
            mResultCode = txINTERNAL_ERROR;
            return false;
        }
    }
    
    int64_t maxWheatReceived = 0;
    if (wheat.type() == NATIVE)
    {
        maxWheatReceived = accountB.mEntry.account().balance;
    }
    else
    {
        maxWheatReceived = wheatLineAccountB.mEntry.trustLine().balance;
    }

    // you can receive the lesser of the amount of wheat offered or the amount the guy has
    if (mWheatTransferRate != TRANSFER_RATE_DIVISOR)
    {
        maxWheatReceived = bigDivide(maxWheatReceived, mWheatTransferRate, TRANSFER_RATE_DIVISOR);
    }
    if (maxWheatReceived > sellingWheatOffer.mEntry.offer().amount)
    {
        maxWheatReceived = sellingWheatOffer.mEntry.offer().amount;
    }

    int64_t numSheepSent;
    int64_t numSheepReceived;
    int64_t numWheatSent;
    int64_t numWheatReceived;

    // this guy can get X wheat to you. How many sheep does that get him?
    int64_t maxSheepBwillBuy = bigDivide(maxWheatReceived,sellingWheatOffer.mEntry.offer().price,OFFER_PRICE_DIVISOR);
    if(maxSheepBwillBuy > maxSheepReceived)
    { // need to only take part of the wheat offer
      // determine how much to take
        numSheepReceived = maxSheepReceived;
        numWheatReceived = bigDivide(numSheepReceived,OFFER_PRICE_DIVISOR,sellingWheatOffer.mEntry.offer().price);
        
        sellingWheatOffer.mEntry.offer().amount -= numWheatReceived;
        mSellSheepOffer.mEntry.offer().amount = 0;
    } else
    { // take whole wheat offer
        numSheepReceived = bigDivide(maxWheatReceived,sellingWheatOffer.mEntry.offer().price,OFFER_PRICE_DIVISOR);
        numWheatReceived = maxWheatReceived;
        sellingWheatOffer.mEntry.offer().amount = 0;
        mSellSheepOffer.mEntry.offer().amount -= numSheepReceived;
    }

    numSheepSent = bigDivide(numSheepReceived,TRANSFER_RATE_DIVISOR,mSheepTransferRate);
    numWheatSent = bigDivide(numWheatReceived,TRANSFER_RATE_DIVISOR,mWheatTransferRate);
 
    if(sellingWheatOffer.mEntry.offer().amount < 0)
    {
        
        CLOG(ERROR, "Tx") << "Transaction::convert offer amount below 0 :" << sellingWheatOffer.mEntry.offer().amount;
        sellingWheatOffer.mEntry.offer().amount = 0;
        // TODO: this is probably a bug, why not throw?
    }

    if(sellingWheatOffer.mEntry.offer().amount)
    {
        sellingWheatOffer.storeChange(delta, ledgerMaster);
    }
    else
    {   // entire offer is taken
        sellingWheatOffer.storeDelete(delta, ledgerMaster);
        accountB.mEntry.account().ownerCount--;
        accountB.storeChange(delta, ledgerMaster);
    }

    // Adjust balances
    if(sheep.type()==NATIVE)
    {
        mSigningAccount->mEntry.account().balance -= numSheepSent;
        accountB.mEntry.account().balance += numSheepSent;

        accountB.storeChange(delta, ledgerMaster);
        mSigningAccount->storeChange(delta, ledgerMaster);
    }
    else
    {
        mSheepLineA.mEntry.trustLine().balance -= numSheepSent;
        sheepLineAccountB.mEntry.trustLine().balance += numSheepReceived;
        sheepLineAccountB.storeChange(delta, ledgerMaster);
        mSheepLineA.storeChange(delta, ledgerMaster);
    }

    if(wheat.type()==NATIVE)
    {
        mSigningAccount->mEntry.account().balance += numWheatSent;
        accountB.mEntry.account().balance -= numWheatSent;
        accountB.storeChange(delta, ledgerMaster);
        mSigningAccount->storeChange(delta, ledgerMaster);
    }
    else
    {
        mWheatLineA.mEntry.trustLine().balance += numWheatReceived;
        wheatLineAccountB.mEntry.trustLine().balance -= numWheatSent;
        wheatLineAccountB.storeChange(delta, ledgerMaster);
        mWheatLineA.storeChange(delta, ledgerMaster);
    }
    return true;
}

// makes sure the currencies are different 
bool CreateOfferFrame::doCheckValid(Application& app)
{
    Currency& sheep = mEnvelope.tx.body.createOfferTx().takerGets;
    Currency& wheat = mEnvelope.tx.body.createOfferTx().takerPays;
    if (compareCurrency(sheep, wheat))
    {
        return false;
    }
    
    return true;
}


}

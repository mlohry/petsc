/*
  formSet - Set Form (hide form if needed)
  input:
    currentAsk
  ouput:
    Form asking questions for currentAsk
*/
function formSet(current)//-1 input for current means that program has finished
{
    if (current=="-1") {//finished asking
        $("#questions").hide();
        finishedAsking=true;
        return;
    }

    $("#currentAskText").html("<b id='currentAskText'>Currently Asking for Matrix A"+current+"</b>");
    $("#posdefRow").hide();
    $("#fieldsplitBlocks").hide();
    $("#fieldsplitBlocks_text").hide();
    $("#symm").removeAttr("checked");
    $("#posdef").removeAttr("checked");
    $("#logstruc").removeAttr("checked");

    if(current == "0")//special case for first node since no defaults were set yet
        return;

    //fill in the form according to defaults set by matTreeGetNextNode
    if (matInfo[getMatIndex(current)].symm) {//if symmetric
        $("#posdefRow").show();
        $("#symm").prop("checked", "true");
    }
    //if posdef, fill in bubble
    if (matInfo[getMatIndex(current)].posdef) {
        $("#posdef").prop("checked", "true");
    }
}

/*
  matTreeGetNextNode - uses matInfo to find and return the id of the next node to ask about SKIP ANY CHILDREN FROM NON-LOG STRUC PARENT
  input:
    currentAsk
  output:
    id of the next node that should be asked
*/
function matTreeGetNextNode(current)
{
    if (current=="0" && askedA0)
        return -1;//sort of base case. this only occurs when the tree has completely finished

    if (current=="0")
        askedA0 = true;

    var parentID  = current.substring(0,current.length-1);//simply knock off the last digit of the id
    var lastDigit = current.charAt(current.length-1);
    lastDigit     = parseInt(lastDigit);

    var currentBlocks = matInfo[getMatIndex(current)].blocks;
    var possibleChild = current+""+(currentBlocks-1);

    //case 1: current node needs more child nodes

    if (matInfo[getMatIndex(current)].logstruc && currentBlocks!=0 && getMatIndex(possibleChild)==-1) {//CHECK TO MAKE SURE CHILDREN DON'T ALREADY EXIST
        //todo: ALL THIS DEFAULT SETTING SHOULD BE DONE ELSEWHERE
        /*var writeLoc = matInfo.length;
        matInfo[writeLoc]        = new Object();
        matInfo[writeLoc].symm   = matInfo[getMatIndex(current)].symm;//set defaults for the new node
        matInfo[writeLoc].posdef = matInfo[getMatIndex(current)].posdef;*/

        return current+"0";//move onto first child
    }

    //case 2: current node's child nodes completed. move on to sister nodes if any

    if (current!="0" && lastDigit+1 < matInfo[getMatIndex(parentID)].blocks) {
        /*var writeLoc = matInfo.length;
        matInfo[writeLoc]        = new Object();
        matInfo[writeLoc].symm   = matInfo[getMatIndex(current)].symm;//set defaults for the new node
        matInfo[writeLoc].posdef = matInfo[getMatIndex(current)].posdef;*/
        var newEnding            = parseInt(lastDigit)+1;

        return ""+parentID+newEnding;
    }

    if (parentID=="")//only happens when there is only one A matrix
        return -1;

    //case 3: recursive case. both current node's child nodes and sister nodes completed. recursive search starting on parent again
    return matTreeGetNextNode(parentID);
}
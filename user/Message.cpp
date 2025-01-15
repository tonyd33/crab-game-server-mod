#include "pch-il2cpp.h"
#include "Message.h"

std::string MessageSendTypeToString(MessageSendType st)
{
	switch (st)
	{
	case MessageSendType::NORMAL:
		return "NORMAL";
	case MessageSendType::FORCE_SEND:
		return "FORCE_SEND";
	case MessageSendType::FORCE_HIDE:
		return "FORCE_HIDE";
	case MessageSendType::FORCE_PRIVATE:
		return "FORCE_PRIVATE";
	default:
		return "UNKNOWN";
	}
}

Message::Message(long long fromClient, std::string content)
{
	FromClientId = fromClient;
	Content = content;
	Init();
}

Message::Message(Player* player, std::string content)
{
	FromPlayer = player;
	FromClientId = player->ClientId;
	Content = content;
	Init();
}

void Message::Init()
{
	if (Content.rfind("!", 0) != 0) return;
	IsCommand = true;

	int i = (int)Content.find(" ");

	Cmd = Content.substr(1, i - 1);
	if (i != -1) CmdArgs = Content.substr(i + 1);

	//std::cout << "CMD=" << Cmd << "|" << std::endl;
	//std::cout << "CMDARGS=" << CmdArgs << "|" << std::endl;
}
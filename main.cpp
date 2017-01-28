#include "Socket.h"
#include "PostOffice.h"
#include "Throttler.h"
#include "ScopeExit.h"
#include "utils.h"
#include "CommandSet.h"

#include "json/src/json.hpp"

#include <sstream>
#include <iostream>
#include <string>
#include <fstream>
#include <thread>
#include <vector>
#include <set>
using namespace std;

int main(void)
{
	CommandSet mCommands;
	Socket twitchConnection;
	Address commandParser("commandParser");
	Address transceiver("transceiver");
	Address pointAdder("pointAdder");
	Address apiAddress("apiServer");

	std::list<std::string> membersInRaffle;
	bool raffleOpen = false;

	auto pointAdderLambda = [pointAdder, commandParser](void) -> void
	{
		Throttler pointIncrementer(std::chrono::seconds(20));
		PostOffice* postOffice = PostOffice::instance();
		Address myAddress = pointAdder;

		std::set<std::string> members = {};
		nlohmann::json points = nlohmann::json::object();

		std::ifstream jsonIn("members.json");
		if (jsonIn.good())
		{
			jsonIn >> points;
		}
		jsonIn.close();

		std::string command = "";
		while (command != "SHUTDOWN")
		{
			if (pointIncrementer.check(1))
			{
				{
					RAIIMutex memberLock(&members);
					for (const std::string& member : members)
					{
						int value = 0;
						if (points[member].is_null())
						{
							points[member] = nlohmann::json::object();
						}
						if (!points[member]["points"].is_null())
						{
							value = points[member]["points"].get<int>();
						}
						points[member]["points"] = ++value;
					}
				}
				pointIncrementer.addUnit();

				std::ofstream jsonIn("members.json");
				if (jsonIn.good())
				{
					jsonIn << points;
				}
				jsonIn.close();
			}

			Message incomingMsg;
			if (PostOffice::isValidInstance(postOffice) && postOffice->checkMail(myAddress))
			{
				incomingMsg = postOffice->getMail(myAddress);
			}
			if (incomingMsg.size() == 0)
			{
				usleep(300);
				continue;
			}

			command = std::string(incomingMsg.raw(), incomingMsg.size() - 1);
			std::vector<std::string> parts = split(command, " ");
			if (parts.empty())
			{
				usleep(300);
				continue;
			}
			command = parts.at(0);
			parts.erase(parts.begin());

			if (command == "JOIN" && !parts.empty())
			{
				members.insert(parts.at(0));
				if (points[parts.at(0)].is_null())
				{
					points[parts.at(0)] = nlohmann::json::object();
				}
				points[parts.at(0)]["role"] = "member";
			}
			else if (command == "PART" && !parts.empty())
			{
				members.erase(members.find(parts.at(0)));
			}
			else if (command == "MODE" && parts.size() >= 2)
			{
				if (points[parts.at(1)].is_null())
				{
					points[parts.at(1)] = nlohmann::json::object();
				}
				points[parts.at(1)]["role"] = parts.at(0);
			}
			else if (command == "GET_POINTS" && !parts.empty())
			{
				std::string user = parts.at(0);
				std::string resp = "0";
				if (!points[user].is_null())
				{
					resp = points[user]["points"].dump();
					Message response(resp);
					incomingMsg.respond(response);
				}
			}
			else if (command == "GETROLE" && !parts.empty())
			{
				if (points[parts.at(0)].is_null())
				{
					points[parts.at(0)] = nlohmann::json::object();
				}

				std::string role = "member";
				if (points[parts.at(0)]["role"].is_string())
				{
					role = points[parts.at(0)]["role"].get<std::string>();
				}
				parts.insert(parts.begin(), role);
				Message msg("RETURNROLE " + join(parts, " "));
				incomingMsg.respond(msg);
			}
			else if (command == "DEDUCT_POINTS" && !parts.empty())
			{
				std::string resp = "DEDUCT_FAIL";
				if (points[parts.at(0)].is_null())
				{
					points[parts.at(0)] = nlohmann::json::object();
				}
				else
				{
					int value = points[parts.at(0)]["points"].get<int>();
					if (value >= 5)
					{
						value -= 5;
						points[parts.at(0)]["points"] = value;
						resp = "DEDUCT_SUCCESS";
					}
				}

				Message msg(resp);
				incomingMsg.respond(msg);
			}
		};
	};
	auto commandParserLambda = [&mCommands, commandParser, pointAdder, transceiver](void) -> void
	{
		PostOffice* postOffice = PostOffice::instance();
		Address myAddress = commandParser;
		SCOPE_EXIT [&](){ cout << "Exiting " << myAddress << endl; };

		Throttler namesTimeout(std::chrono::seconds(60));
		std::string command = "";
		std::string prefix = "";
		std::vector<std::string> params;

		while (command != "SHUTDOWN")
		{
			Message incomingMsg;
			if (PostOffice::isValidInstance(postOffice) && postOffice->checkMail(myAddress))
			{
				incomingMsg = postOffice->getMail(myAddress);
			}

			if (incomingMsg.size() == 0)
			{
				usleep(300);
				continue;
			}
			std::string role = "";
			CommandParts cmdParts = parseCommand(incomingMsg);
			if (!cmdParts.valid)
			{
				continue;
			}
			command = cmdParts.command;
			prefix = cmdParts.prefix;
			params = cmdParts.params;

			cout << "Prefix='" << prefix << "' Command='" << command << "' Params=['" << join(params, "', '") << "']" << endl;
			if (command == "PING")
			{
				Message msg("PONG " + join(params, " ") + "\r\n");
				msg.send(myAddress, transceiver);
			}
			else if (command == "PRIVMSG")
			{
				std::string user = split(prefix.substr(1), "!").at(0);
				std::string channel = params.front();
				params.erase(params.begin());

				if (!mCommands.run(user, channel, params))
				{
					cout << "Unable to run command..." << endl;
				}
				else
				{
					cout << "Command ran succesffully!" << endl;
				}
			}
			else if (command == "JOIN" || command == "PART")
			{
				std::string user = split(prefix.substr(1), "!").at(0);
				Message msg(command + " " + user);
				msg.send(myAddress, pointAdder);
			}
			else if (command == "MODE" && params.size() >= 3)
			{
				Message msg("MODE " + params.at(1) + " " + params.at(2));
				msg.send(myAddress, pointAdder);
			}
			else if (command == "376") // the end of the MOTD message.
			{
				Message msg("CAP REQ twitch.tv/membership\r\n");
				msg.send(myAddress, transceiver);
				msg = Message("JOIN #betawar1305\r\n");
				msg.send(myAddress, transceiver);
			}
			else if (command == "353") // members list
			{
				std::vector<std::string> names = split(params.back(), " ");
				for (const std::string& name : names)
				{
					Message msg("JOIN " + name);
					msg.send(myAddress, pointAdder);
				}
			}
			else if (command == "366") // member list finished
			{
				// empty
			}
		}
	};

	auto transceiverLambda = [&twitchConnection, commandParser, transceiver, pointAdder, apiAddress](void) -> void
	{
		const int bufferLength = 1024;
		char* buffer = new char[bufferLength];
		Throttler throttle;
		PostOffice* postOffice = PostOffice::instance();
		Address myAddress = transceiver;

		SCOPE_EXIT [&](void) -> void
		{
			delete [] buffer;
			Message msg("SHUTDOWN");
			msg.send(myAddress, commandParser);
			msg.send(myAddress, pointAdder);
			msg.send(myAddress, apiAddress);
			cout << "Exiting " << myAddress << endl;
		};

		std::string incompleteMessage = "";
		while (twitchConnection.connected())
		{
			if (PostOffice::isValidInstance(postOffice) && postOffice->checkMail(myAddress) && throttle.check(20))
			{
				Message receivedMessage = postOffice->getMail(myAddress);

				std::size_t size = receivedMessage.size();
				std::size_t sentBytes = 0;
				const char* ptr = receivedMessage.raw();
				std::string s(ptr, size);
				//if (s.find("PASS") == std::string::npos)
				//{
				//	cout << "Sending message: '" << ptr << "' to twitch." << endl;
				//}

				do
				{
					int sent = twitchConnection.send(ptr + sentBytes, size - sentBytes, 100);

					if (sent < 0)
					{
						cout << "Something bad happened while sending message" << endl;
						break;
					}

					sentBytes += sent;
				} while (sentBytes < size);
				throttle.addUnit();
			}

			int receivedBytes = twitchConnection.receive(buffer, bufferLength - 1);
			if (receivedBytes == 0)
			{
				usleep(30);
				continue;
			}
			buffer[receivedBytes] = '\0';

			std::string currentMessage = "";
			std::stringstream s(buffer);
			do
			{
				std::getline(s, currentMessage, '\n');
				incompleteMessage += currentMessage;

				if (!incompleteMessage.empty() && incompleteMessage.back() == '\r')
				{
					incompleteMessage = incompleteMessage.substr(0, incompleteMessage.size() - 1);
					Message msg(incompleteMessage);
					msg.send(myAddress, commandParser);
					incompleteMessage = "";
				}
			} while (!s.eof());
		}

	};

	std::list<Socket*> clients;
	auto apiServerLambda = [&clients, apiAddress](void) -> void
	{
		Address myAddress = apiAddress;
		Socket server;
		int ret = 0;

		do
		{
			usleep(300);
			ret = server.bind("60000", "127.0.0.1");
		} while (ret == EAGAIN);

		while (server.connected())
		{
			if (PostOffice::instance()->checkMail(myAddress))
			{
				break;
			}

			Socket* client = server.accept();
			if (client && client->connected())
			{
				RAIIMutex clientsLock(&clients);
				clients.push_back(client);
			}
		}
		server.close();
	};

	auto apiLoopLambda = [&clients, apiAddress, transceiver](void) -> void
	{
		Address myAddress = apiAddress;
		std::size_t bufferLength = 1024;
		char* buffer = new char[bufferLength];
		Socket* client;

		while (true)
		{
			if (PostOffice::instance()->checkMail(myAddress))
			{
				break;
			}
			if (clients.empty())
			{
				usleep(300);
				continue;
			}

			{
				RAIIMutex clientsLock(&clients);
				client = clients.front();
				clients.pop_front();
			}
			
			if (!client->connected())
			{
				delete client;
				continue;
			}

			int receivedBytes = client->receive(buffer, bufferLength - 1);

			{
				RAIIMutex clientsLock(&clients);
				clients.push_back(client);
			}

			if (receivedBytes == 0)
			{
				usleep(30);
				continue;
			}
			buffer[receivedBytes] = '\0';

			std::string message = "PRIVMSG #betawar1305 :" + string(buffer, receivedBytes) + "\r\n";
			Message msg(message);
			msg.send(myAddress, transceiver);
		}

		delete [] buffer;
		RAIIMutex clientsLock(&clients);
		for (Socket*& client : clients)
		{
			delete client;
			client = NULL;
		}
	};


	mCommands.registerCommand({"!shutdown", [&](const std::string& user, const std::string& channel, const std::vector<std::string>& params) -> void {
		if (user == "betawar1305")
		{
			Message msg("SHUTDOWN");
			msg.send(commandParser, transceiver);
		}
	}});

	mCommands.registerCommand({"!points", [&](const std::string& user, const std::string& channel, const std::vector<std::string>& params) -> void {
		nlohmann::json points = nlohmann::json::object();
		std::ifstream jsonIn("members.json");
		if (jsonIn.good())
		{
			jsonIn >> points;
		}
		jsonIn.close();

		Message pointInfo("GET_POINTS " + user);
		Message response = pointInfo.sendAndAwaitResponse(commandParser, pointAdder);
		if (response.size() == 0)
		{
			Message msg("PRIVMSG " + channel + " :@" + user + ", your currently have no points.\r\n");
			msg.send(commandParser, transceiver);
		}
		std::string pointValue(response.raw(), response.size());

		cout << "Retrieving points for '" << user << "' on " << channel << endl;
		if (pointValue == "0")
		{
			Message msg("PRIVMSG " + channel + " :@" + user + ", your currently have no points.\r\n");
			msg.send(commandParser, transceiver);
		}
		else
		{
			Message msg("PRIVMSG " + channel + " :@" + user + ", your point total is " + pointValue + ".\r\n");
			msg.send(commandParser, transceiver);
		}
	}});

	mCommands.registerCommand({"!startRaffle", [&](const std::string& user, const std::string& channel, const std::vector<std::string>& params) -> void {
		Message msg("GETROLE " + user + " PRIVMSG " + join(params, " "));
		Message response = msg.sendAndAwaitResponse(commandParser, pointAdder);
		if (response.size() == 0)
		{
			return;
		}
		CommandParts cmdParts = parseCommand(response);
		if (!cmdParts.valid)
		{
			return;
		}

		std::string command = cmdParts.command;
		std::string prefix = cmdParts.prefix;
		std::vector<std::string> parms = cmdParts.params;
		std::string role = parms.at(0);
		cout << "=== Prefix='" << prefix << "' Command='" << command << "' Params=['" << join(parms, "', '") << "'], Role='" << role << "'" << endl;

		if (role == "+o")
		{
			raffleOpen = true;
			Message msg("PRIVMSG " + channel + " :The raffle is now open! Send '!joinRaffle' (no quotes) to join.\r\n");
			msg.send(commandParser, transceiver);
		}
		else
		{
			Message msg("PRIVMSG " + channel + " :@" + user + ", you don't have permissions to execute that command.\r\n");
			msg.send(commandParser, transceiver);
		}
	}});

	mCommands.registerCommand({"!joinRaffle", [&](const std::string& user, const std::string& channel, const std::vector<std::string>& params) -> void {
		/*
		 * NOTE This isn't done per-say. It has a drastic flaw in that it doesn't
		 *      actually cost points to join the raffle. We will want to change
		 *      that. But given that I don't really like the way the asynchronous
		 *      messaging is working out with the GETROLE side of things, I am
		 *      going to take some time to architect this and come back with a
		 *      better option.
		 */
		Message msg("DEDUCT_POINTS " + user);
		Message response = msg.sendAndAwaitResponse(commandParser, pointAdder);
		if (response.size() == 0)
		{
			return;
		}
		CommandParts cmdParts = parseCommand(response);
		if (!cmdParts.valid)
		{
			return;
		}
		std::string command = cmdParts.command;
		std::string prefix = cmdParts.prefix;
		std::vector<std::string> parms = cmdParts.params;
		if (command == "DEDUCT_SUCCESS")
		{
			membersInRaffle.push_back(user);
			Message msg("PRIVMSG " + channel + " :@" + user + ", you are now in the raffle.\r\n");
			msg.send(commandParser, transceiver);
		}
		else
		{
			Message msg("PRIVMSG " + channel + " :@" + user + ", Sorry, but the command timed out... Please try again.\r\n");
			msg.send(commandParser, transceiver);
		}
	}});

	int connectionRet = twitchConnection.connect("irc.chat.twitch.tv", "6667");
	cout << "Connection return value: " << connectionRet << endl;

	ifstream fileIn("/home/james/.twitch_oauth.key");
	std::string oauthToken = "";
	if (!fileIn.is_open())
	{
		cout << "ERROR: Unable to open file..." << endl;
	}
	fileIn >> oauthToken;
	fileIn.close();

	Message pass("PASS " + oauthToken + "\r\n");
	pass.send(transceiver, transceiver);
	Message nick("NICK betawar1305\r\n");
	nick.send(transceiver, transceiver);

	std::list<std::thread> threads;
	threads.push_back(std::thread(transceiverLambda));
	threads.push_back(std::thread(commandParserLambda));
	threads.push_back(std::thread(pointAdderLambda));
	threads.push_back(std::thread(apiServerLambda));
	threads.push_back(std::thread(apiLoopLambda));

	for (std::thread& t : threads)
	{
		t.join();
	}

	twitchConnection.close();
	PostOffice::finalize();

	return 0;
}

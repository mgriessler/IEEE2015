import cv2
import numpy as np 
import math
import Rotate
import baseLine
import findCoordinates
import Squares
import edgechesshc
import FindPiece
import chess_ai

booleanArray = [True, True, True, True,True, True, True, True,
				True, True, True, True,True, True, True, True, 
				False, False, False, False, False, False, False, False,
				False, False, False, False,False, False, False, False,
				False, False, False, False,False, False, False, False,
				False, False, False, False,False, False, False, False,
				True, True, True, True,True, True, True, True,
				True, True, True, True,True, True, True, True]

pieceTypeArray = ['r','n','b','q','k','b','n','r',
				  'p','p','p','p','p','p','p','p',
				  'e','e','e','e','e','e','e','e',
				  'e','e','e','e','e','e','e','e',
				  'e','e','e','e','e','e','e','e',
				  'e','e','e','e','e','e','e','e',
				  'P','P','P','P','P','P','P','P',
				  'R','N','B','Q','K','B','N','R']

finalFE = ""
w_or_b_turn = 'w'
ROBOTCOLOR = True
oldboard = [] #oldboard is our one for reference that will constantly be changed into 
newboard = []

def showImage(windowName, imageName):
	cv2.namedWindow(windowName, cv2.WINDOW_NORMAL)
	cv2.imshow(windowName, imageName)


def main():
	#######################initialize##############################
	#Take in image file
	'''
	take in file img to initialize
	then run it throught the same 
	'''

	filename = 'lib/board.jpg'
	img = cv2.imread(filename)
	#newimage = cv2.resize(img, (640,480))
	showImage('img', img)
	cv2.waitKey(0)

	gray = cv2.imread(filename, 0)

	#Get the angle of rotation needed and rotate
	theta = baseLine.findAngle(filename)
	rotatedImage = Rotate.rotateImage(theta, img)

	showImage('rotated image', rotatedImage)
	cv2.waitKey(0)
	cv2.destroyAllWindows()
	#Save the rotated image
	cv2.imwrite('lib/rotated.jpg', rotatedImage)

	#Find all the lines of the squares
	imgWithEdges = edgechesshc.addEdges('lib/rotated.jpg')

	#Get all the coordinates of the keypoints
	coordinates = findCoordinates.getCoordinates(imgWithEdges)

	#Pass coordinates on to Squares

	squares = Squares.populateSquares(coordinates)
	
	#Load squares with their starting state
	Squares.loadPieces(squares, booleanArray, pieceTypeArray)

	

	########################repeatedProcess############################
	
	#at the end of all repeated processes we need to shift all the "new"
	#stuff into the "old" stuff we are using for reference and comparison
	#Find where the peices lie on the board

	#replace with updated image
	newImage = cv2.imread('lib/rotated.jpg')

	
	squaresCropped = Squares.getSquaresCropped(squares, newImage)
	
	print type(squaresCropped)

	arrayOfBooleans, centerOfPiece, array_of_colors = FindPiece.main(squaresCropped)
	#the arrayOfBooleans is T or F if there is a piece in a square
	#centerofpeice is an array of coordinates for the cetner of each piece (catch is it has to be perfectly centered on top of the piece)


	print arrayOfBooleans
	print centerOfPiece
	print array_of_colors

	print pieceTypeArray
	#Manage old and new images first
	#pieceTypeArray = findDifferencesArrays(oldBool, newBool, pieceType)
	
	####################################finalProcess to start up again####################################3
	####We have to come up with a way to determine who moved, white or balck so that we can send it to AI
	###########we will have to constantly update piecetype array as well and boolAray
	finalFE = Squares.giveForsythEdwardsNotation(pieceTypeArray, w_or_b_turn)

	ai_move = chess_ai.get_chess_move(finalFE, ROBOTCOLOR)  

	'''
	##############How to do this#####################################
	if (w_or_b_turn == 'w'):
		w_or_b_turn == 'b'
	elif(w_or_b_turn == 'b'):
		w_or_b_turn == 'w'
		'''

	print ai_move
	############################Testing##############################
	#Show all the important images
	showImage('original', img)
	showImage('rotated', rotatedImage)
	showImage('image with edges', imgWithEdges)


	#Show all the cropped images
	#Squares.printCroppedSquares(squaresCropped)

	cv2.waitKey(0)
	cv2.destroyAllWindows()


main()

